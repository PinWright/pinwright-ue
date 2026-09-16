// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Utility domain handlers
// Covers MiscHandler.cpp and UtilityPropertyHandler.cpp.
// system.console_command lives in System/SystemControlHandler.cpp;
// its tests remain here for historical grouping.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Components/SceneComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Compat/EngineVersionCompat.h"
// FOverridableManager (Overridable Serialization) arrived in UE 5.4; the header is absent on
// 5.3. The override-clearing test below is gated to engines that ship the manager.
#if __has_include("UObject/OverridableManager.h")
#include "UObject/OverridableManager.h"
#define MCP_HAS_OVERRIDABLE_MANAGER 1
#else
#define MCP_HAS_OVERRIDABLE_MANAGER 0
#endif
#if __has_include("UObject/PropertyVisitor.h")
#include "UObject/PropertyVisitor.h"
#define MCP_HAS_PROPERTY_VISITOR 1
#else
#define MCP_HAS_PROPERTY_VISITOR 0
#endif


// ============================================================================
// system.console_command
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleCommandValidParamsTest,
    "PinWright.system.console_command.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleCommandValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("command"), TEXT("stat unit"));
    TestTrue(TEXT("system.console_command invoked"), InvokeHandler(TEXT("system.console_command"), Payload));
    return true;
}

// ============================================================================
// misc.create_post_process_volume
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMiscCreatePostProcessVolumeValidParamsTest,
    "PinWright.misc.create_post_process_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMiscCreatePostProcessVolumeValidParamsTest::RunTest(const FString& Parameters)
{
    // No required params
    TestTrue(TEXT("misc.create_post_process_volume invoked"), InvokeHandler(TEXT("misc.create_post_process_volume"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// misc.create_camera
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMiscCreateCameraValidParamsTest,
    "PinWright.misc.create_camera.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMiscCreateCameraValidParamsTest::RunTest(const FString& Parameters)
{
    // No required params
    TestTrue(TEXT("misc.create_camera invoked"), InvokeHandler(TEXT("misc.create_camera"), MakeShared<FJsonObject>()));
    return true;
}

// Regression guard for E-create-camera-cine-doc: the summary advertised
// "(or CineCameraActor)" but the handler unconditionally spawned a plain
// ACameraActor and rejected every cine selector with [UNKNOWN_PARAMS]. The fix
// added a cine opt-in (cine=true / cameraClass="cine"). These assert (a) the
// default still spawns a plain CameraActor and (b) the opt-in spawns the cine
// variant — both would fail if the class-selection branch were reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMiscCreateCameraCineVariantTest,
    "PinWright.misc.create_camera.CineVariantSpawnsCineCameraActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMiscCreateCameraCineVariantTest::RunTest(const FString& Parameters)
{
    // Builds a create_camera payload with the given label and an optional selector applied.
    auto MakePayload = [](const TCHAR* Name, TFunctionRef<void(FJsonObject&)> SetSelector)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("cameraName"), Name);
        SetSelector(*Payload);
        return Payload;
    };

    // Default (no cine selector): plain CameraActor.
    TestSpawnHandlerProducesActorClass(*this, TEXT("misc.create_camera"),
        MakePayload(TEXT("RegressionPlainCam"), [](FJsonObject&) {}),
        FString(TEXT("CameraActor")));

    // cine=true: the documented CineCameraActor variant must actually spawn one.
    TestSpawnHandlerProducesActorClass(*this, TEXT("misc.create_camera"),
        MakePayload(TEXT("RegressionCineCam"), [](FJsonObject& P) { P.SetBoolField(TEXT("cine"), true); }),
        FString(TEXT("CineCameraActor")));

    // cameraClass="cine" alias resolves to the same cine variant.
    TestSpawnHandlerProducesActorClass(*this, TEXT("misc.create_camera"),
        MakePayload(TEXT("RegressionCineCamAlias"), [](FJsonObject& P) { P.SetStringField(TEXT("cameraClass"), TEXT("cine")); }),
        FString(TEXT("CineCameraActor")));

    return true;
}

// ============================================================================
// misc.set_camera_fov
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMiscSetCameraFovValidParamsTest,
    "PinWright.misc.set_camera_fov.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMiscSetCameraFovValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("cameraName"), TEXT("NonExistentCamera"));
    Payload->SetNumberField(TEXT("fov"), 75.0);
    TestTrue(TEXT("misc.set_camera_fov invoked"), InvokeHandler(TEXT("misc.set_camera_fov"), Payload));
    return true;
}

// ============================================================================
// misc.set_game_speed
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMiscSetGameSpeedValidParamsTest,
    "PinWright.misc.set_game_speed.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMiscSetGameSpeedValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("speed"), 1.0);
    TestTrue(TEXT("misc.set_game_speed invoked"), InvokeHandler(TEXT("misc.set_game_speed"), Payload));
    return true;
}

// ============================================================================
// misc.set_replication
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMiscSetReplicationValidParamsTest,
    "PinWright.misc.set_replication.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMiscSetReplicationValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/NonExistentBlueprint"));
    Payload->SetBoolField(TEXT("replicates"), true);
    TestTrue(TEXT("misc.set_replication invoked"), InvokeHandler(TEXT("misc.set_replication"), Payload));
    return true;
}

// ============================================================================
// property.set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetValidParamsTest,
    "PinWright.property.set.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("bHidden"));
    Payload->SetBoolField(TEXT("value"), true);
    TestTrue(TEXT("property.set invoked"), InvokeHandler(TEXT("property.set"), Payload));
    return true;
}

// ============================================================================
// property.get
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyGetValidParamsTest,
    "PinWright.property.get.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyGetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("bHidden"));
    TestTrue(TEXT("property.get invoked"), InvokeHandler(TEXT("property.get"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyGetWithDefaultsAndOverridesTest,
    "PinWright.property.get.WithDefaultsAndOverridesNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyGetWithDefaultsAndOverridesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("bHidden"));
    Payload->SetBoolField(TEXT("includeDefault"), true);
    Payload->SetBoolField(TEXT("includeOverrideState"), true);
    Payload->SetBoolField(TEXT("includeMetadata"), true);
    TestTrue(TEXT("property.get (extended flags) invoked"), InvokeHandler(TEXT("property.get"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyGetOmitOversizedReturnsPlaceholderTest,
    "PinWright.property.get.OmitOversizedReturnsPlaceholder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyGetOmitOversizedReturnsPlaceholderTest::RunTest(const FString& Parameters)
{
    UInstancedStaticMeshComponent* Component =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient instanced static mesh component created"), Component);
    if (!Component)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Component->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("PerInstanceSMData"));
    Payload->SetBoolField(TEXT("omitOversized"), true);

    FTestResponseCapture Capture;
    TestTrue(
        TEXT("property.get handler found"),
        InvokeHandlerWithCapture(TEXT("property.get"), Payload, Capture));
    TestTrue(TEXT("property.get responded"), Capture.bWasCalled);
    TestTrue(TEXT("property.get succeeded"), Capture.bSuccess);
    TestTrue(TEXT("property.get returned a payload"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* ValueObject = nullptr;
    TestTrue(
        TEXT("result.value is a placeholder object"),
        Capture.Result->TryGetObjectField(TEXT("value"), ValueObject) &&
        ValueObject && ValueObject->IsValid());
    if (!ValueObject || !ValueObject->IsValid())
    {
        return false;
    }

    FString Reason;
    TestTrue(
        TEXT("placeholder reason is exceeds-llm-budget"),
        (*ValueObject)->TryGetStringField(TEXT("$reason"), Reason) &&
        Reason == TEXT("exceeds-llm-budget"));

    FString Type;
    TestTrue(
        TEXT("placeholder type is TArray<FInstancedStaticMeshInstanceData>"),
        (*ValueObject)->TryGetStringField(TEXT("type"), Type) &&
        Type == TEXT("TArray<FInstancedStaticMeshInstanceData>"));

    return true;
}

// Overridable Serialization (FOverridableManager) only exists on UE 5.4+. On 5.3 this entire
// test is compiled out; property.reset still works there as a plain value reset.
#if MCP_HAS_OVERRIDABLE_MANAGER
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyResetClearsExplicitOverrideTest,
    "PinWright.property.reset.ClearsExplicitOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyResetClearsExplicitOverrideTest::RunTest(const FString& Parameters)
{
    USceneComponent* Component = NewObject<USceneComponent>(
        GetTransientPackage(), USceneComponent::StaticClass(), NAME_None, RF_Transactional);
    TestNotNull(TEXT("Transient scene component created"), Component);
    if (!Component)
    {
        return false;
    }

    FProperty* RelativeLocationProperty =
        USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("USceneComponent has RelativeLocation"), RelativeLocationProperty);
    if (!RelativeLocationProperty)
    {
        return false;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // UE 5.6+: the manager is lazily constructed; TryGet()/Create() gate access,
    // and Enable() takes TNotNull<UObject*> (the raw Component pointer).
    if (!FOverridableManager::TryGet())
    {
        FOverridableManager::Create();
    }
    FOverridableManager& OverrideManager = FOverridableManager::Get();
    OverrideManager.Enable(Component);
#else
    // UE 5.5 and 5.4: Get() is a static singleton (no TryGet/Create) that is always
    // available, and Enable() takes a UObject& reference.
    FOverridableManager& OverrideManager = FOverridableManager::Get();
    OverrideManager.Enable(*Component);
#endif

    *RelativeLocationProperty->ContainerPtrToValuePtr<FVector>(Component) =
        FVector(10.0, 20.0, 30.0);

#if MCP_HAS_PROPERTY_VISITOR
    // UE 5.5+: use FPropertyVisitorPath to mark the property as explicitly overridden
    // and to verify the override state before/after the reset. The accessor argument
    // form differs by version: UE 5.6 takes TNotNull<UObject*> (the Component pointer),
    // UE 5.5 takes a UObject& reference.
    const FPropertyVisitorPath PropertyPath{FPropertyVisitorInfo(RelativeLocationProperty)};
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    OverrideManager.OverrideProperty(Component, PropertyPath);

    TestNotEqual(
        TEXT("RelativeLocation starts explicitly overridden"),
        OverrideManager.GetOverriddenPropertyOperation(Component, PropertyPath),
        EOverriddenPropertyOperation::None);
#else
    OverrideManager.OverrideProperty(*Component, PropertyPath);

    TestNotEqual(
        TEXT("RelativeLocation starts explicitly overridden"),
        OverrideManager.GetOverriddenPropertyOperation(*Component, PropertyPath),
        EOverriddenPropertyOperation::None);
#endif
#else
    // UE 5.4: use FPropertyChangedEvent + FEditPropertyChain for the override API.
    FEditPropertyChain PropertyChainForTest;
    PropertyChainForTest.AddTail(RelativeLocationProperty);
    PropertyChainForTest.SetActivePropertyNode(RelativeLocationProperty);
    PropertyChainForTest.SetActiveMemberPropertyNode(RelativeLocationProperty);
    FPropertyChangedEvent PropertyEventForTest(RelativeLocationProperty, EPropertyChangeType::ValueSet);
    OverrideManager.OverrideProperty(*Component, PropertyEventForTest, PropertyChainForTest);

    TestNotEqual(
        TEXT("RelativeLocation starts explicitly overridden"),
        OverrideManager.GetOverriddenPropertyOperation(*Component, PropertyEventForTest, PropertyChainForTest),
        EOverriddenPropertyOperation::None);
#endif // MCP_HAS_PROPERTY_VISITOR

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Component->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("RelativeLocation"));
    Payload->SetBoolField(TEXT("markDirty"), false);

    FTestResponseCapture Capture;
    TestTrue(
        TEXT("property.reset handler found"),
        InvokeHandlerWithCapture(TEXT("property.reset"), Payload, Capture));
    TestTrue(TEXT("property.reset succeeded"), Capture.bSuccess);
    TestTrue(TEXT("property.reset returned a payload"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bWasOverridden = false;
    TestTrue(
        TEXT("response includes wasOverridden"),
        Capture.Result->TryGetBoolField(TEXT("wasOverridden"), bWasOverridden));
    TestTrue(TEXT("response reports prior explicit override"), bWasOverridden);

    bool bIsOverridden = true;
    TestTrue(
        TEXT("response includes isOverridden"),
        Capture.Result->TryGetBoolField(TEXT("isOverridden"), bIsOverridden));
    TestFalse(TEXT("response reports final override state cleared"), bIsOverridden);

#if MCP_HAS_PROPERTY_VISITOR
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // UE 5.6+: accessor takes TNotNull<UObject*> (the Component pointer).
    TestEqual(
        TEXT("FOverridableManager reports no explicit override after reset"),
        OverrideManager.GetOverriddenPropertyOperation(Component, PropertyPath),
        EOverriddenPropertyOperation::None);
#else
    // UE 5.5: accessor takes a UObject& reference.
    TestEqual(
        TEXT("FOverridableManager reports no explicit override after reset"),
        OverrideManager.GetOverriddenPropertyOperation(*Component, PropertyPath),
        EOverriddenPropertyOperation::None);
#endif
#else
    TestEqual(
        TEXT("FOverridableManager reports no explicit override after reset"),
        OverrideManager.GetOverriddenPropertyOperation(*Component, PropertyEventForTest, PropertyChainForTest),
        EOverriddenPropertyOperation::None);
#endif // MCP_HAS_PROPERTY_VISITOR

    const USceneComponent* DefaultComponent =
        CastChecked<USceneComponent>(USceneComponent::StaticClass()->GetDefaultObject());
    const void* CurrentValue =
        RelativeLocationProperty->ContainerPtrToValuePtr<void>(Component);
    const void* DefaultValue =
        RelativeLocationProperty->ContainerPtrToValuePtr<void>(DefaultComponent);
    TestTrue(
        TEXT("current value is identical to default pointer value"),
        RelativeLocationProperty->Identical(CurrentValue, DefaultValue, PPF_None));

    return true;
}
#endif // MCP_HAS_OVERRIDABLE_MANAGER

// ============================================================================
// property.list
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyListValidParamsTest,
    "PinWright.property.list.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyListValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("NonExistentObject"));
    Payload->SetBoolField(TEXT("includeAll"), false);
    Payload->SetBoolField(TEXT("includeReadOnly"), false);
    Payload->SetBoolField(TEXT("includeValues"), true);
    Payload->SetBoolField(TEXT("includeDefault"), true);
    Payload->SetBoolField(TEXT("includeOverrideState"), true);
    Payload->SetBoolField(TEXT("includeMetadata"), true);
    TestTrue(TEXT("property.list invoked"), InvokeHandler(TEXT("property.list"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyListOmitOversizedReturnsPlaceholderTest,
    "PinWright.property.list.OmitOversizedReturnsPlaceholder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyListOmitOversizedReturnsPlaceholderTest::RunTest(const FString& Parameters)
{
    UInstancedStaticMeshComponent* Component =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient instanced static mesh component created"), Component);
    if (!Component)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Component->GetPathName());
    Payload->SetBoolField(TEXT("includeAll"), true);
    Payload->SetBoolField(TEXT("includeValues"), true);
    Payload->SetBoolField(TEXT("includeDefault"), false);
    Payload->SetBoolField(TEXT("omitOversized"), true);

    FTestResponseCapture Capture;
    TestTrue(
        TEXT("property.list handler found"),
        InvokeHandlerWithCapture(TEXT("property.list"), Payload, Capture));
    TestTrue(TEXT("property.list responded"), Capture.bWasCalled);
    TestTrue(TEXT("property.list succeeded"), Capture.bSuccess);
    TestTrue(TEXT("property.list returned a payload"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
    TestTrue(
        TEXT("property.list returned properties array"),
        Capture.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties);
    if (!Properties)
    {
        return false;
    }

    TSharedPtr<FJsonObject> PerInstanceDataEntry;
    for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
    {
        TSharedPtr<FJsonObject> PropertyObject =
            PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
                ? PropertyValue->AsObject()
                : nullptr;
        if (!PropertyObject.IsValid())
        {
            continue;
        }

        FString PropertyName;
        if (PropertyObject->TryGetStringField(TEXT("name"), PropertyName) &&
            PropertyName == TEXT("PerInstanceSMData"))
        {
            PerInstanceDataEntry = PropertyObject;
            break;
        }
    }

    TestTrue(TEXT("property.list contains PerInstanceSMData"), PerInstanceDataEntry.IsValid());
    if (!PerInstanceDataEntry.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* ValueObject = nullptr;
    TestTrue(
        TEXT("PerInstanceSMData value is a placeholder object"),
        PerInstanceDataEntry->TryGetObjectField(TEXT("value"), ValueObject) &&
        ValueObject && ValueObject->IsValid());
    if (!ValueObject || !ValueObject->IsValid())
    {
        return false;
    }

    FString Reason;
    TestTrue(
        TEXT("placeholder reason is exceeds-llm-budget"),
        (*ValueObject)->TryGetStringField(TEXT("$reason"), Reason) &&
        Reason == TEXT("exceeds-llm-budget"));

    return true;
}

// ============================================================================
// property.list — Blueprint CDO auto-resolution
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyListBlueprintCDOResolutionTest,
    "PinWright.property.list.BlueprintCDOResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyListBlueprintCDOResolutionTest::RunTest(const FString& Parameters)
{
    // Create a transient test Blueprint so we have a known asset to resolve
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        TEXT("TestBP_CDOResolution"),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!BP || !BP->GeneratedClass)
    {
        AddError(TEXT("Failed to create test Blueprint"));
        return true;
    }

    // Call property.list with the Blueprint's path (not the CDO path)
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("includeAll"), true);
    Payload->SetBoolField(TEXT("includeValues"), true);

    bool bFound = InvokeHandlerWithCapture(TEXT("property.list"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // className should be the generated class name (Actor-derived), not "Blueprint"
        FString ClassName;
        if (Capture.Result->TryGetStringField(TEXT("className"), ClassName))
        {
            TestFalse(TEXT("className should NOT be 'Blueprint'"),
                ClassName.Equals(TEXT("Blueprint")));
            TestTrue(TEXT("className should contain the BP generated class"),
                ClassName.Contains(TEXT("TestBP_CDOResolution")));
        }
    }

    return true;
}

// ============================================================================
// container.array.append
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayAppendValidParamsTest,
    "PinWright.container.array.append.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayAppendValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyArray"));
    Payload->SetStringField(TEXT("value"), TEXT("element"));
    TestTrue(TEXT("container.array.append invoked"), InvokeHandler(TEXT("container.array.append"), Payload));
    return true;
}

// ============================================================================
// container.array.remove
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayRemoveValidParamsTest,
    "PinWright.container.array.remove.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayRemoveValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyArray"));
    Payload->SetNumberField(TEXT("index"), 0.0);
    TestTrue(TEXT("container.array.remove invoked"), InvokeHandler(TEXT("container.array.remove"), Payload));
    return true;
}

// ============================================================================
// container.array.clear
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayClearValidParamsTest,
    "PinWright.container.array.clear.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayClearValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyArray"));
    TestTrue(TEXT("container.array.clear invoked"), InvokeHandler(TEXT("container.array.clear"), Payload));
    return true;
}

// ============================================================================
// container.array.insert
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayInsertValidParamsTest,
    "PinWright.container.array.insert.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayInsertValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyArray"));
    Payload->SetNumberField(TEXT("index"), 0.0);
    Payload->SetStringField(TEXT("value"), TEXT("element"));
    TestTrue(TEXT("container.array.insert invoked"), InvokeHandler(TEXT("container.array.insert"), Payload));
    return true;
}

// ============================================================================
// container.array.get
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayGetValidParamsTest,
    "PinWright.container.array.get.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayGetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyArray"));
    Payload->SetNumberField(TEXT("index"), 0.0);
    TestTrue(TEXT("container.array.get invoked"), InvokeHandler(TEXT("container.array.get"), Payload));
    return true;
}

// ============================================================================
// container.array.set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArraySetValidParamsTest,
    "PinWright.container.array.set.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArraySetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyArray"));
    Payload->SetNumberField(TEXT("index"), 0.0);
    Payload->SetStringField(TEXT("value"), TEXT("element"));
    TestTrue(TEXT("container.array.set invoked"), InvokeHandler(TEXT("container.array.set"), Payload));
    return true;
}

// ============================================================================
// container.map.set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapSetValidParamsTest,
    "PinWright.container.map.set.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapSetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyMap"));
    Payload->SetStringField(TEXT("key"), TEXT("KeyA"));
    Payload->SetStringField(TEXT("value"), TEXT("ValueA"));
    TestTrue(TEXT("container.map.set invoked"), InvokeHandler(TEXT("container.map.set"), Payload));
    return true;
}

// ============================================================================
// container.map.get
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapGetValidParamsTest,
    "PinWright.container.map.get.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapGetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyMap"));
    Payload->SetStringField(TEXT("key"), TEXT("KeyA"));
    TestTrue(TEXT("container.map.get invoked"), InvokeHandler(TEXT("container.map.get"), Payload));
    return true;
}

// ============================================================================
// container.map.remove
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapRemoveValidParamsTest,
    "PinWright.container.map.remove.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapRemoveValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyMap"));
    Payload->SetStringField(TEXT("key"), TEXT("KeyA"));
    TestTrue(TEXT("container.map.remove invoked"), InvokeHandler(TEXT("container.map.remove"), Payload));
    return true;
}

// ============================================================================
// container.map.has_key
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapHasKeyValidParamsTest,
    "PinWright.container.map.has_key.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapHasKeyValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyMap"));
    Payload->SetStringField(TEXT("key"), TEXT("KeyA"));
    TestTrue(TEXT("container.map.has_key invoked"), InvokeHandler(TEXT("container.map.has_key"), Payload));
    return true;
}

// ============================================================================
// container.map.get_keys
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapGetKeysValidParamsTest,
    "PinWright.container.map.get_keys.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapGetKeysValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyMap"));
    TestTrue(TEXT("container.map.get_keys invoked"), InvokeHandler(TEXT("container.map.get_keys"), Payload));
    return true;
}

// ============================================================================
// container.map.clear
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapClearValidParamsTest,
    "PinWright.container.map.clear.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapClearValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MyMap"));
    TestTrue(TEXT("container.map.clear invoked"), InvokeHandler(TEXT("container.map.clear"), Payload));
    return true;
}

// ============================================================================
// container.set.add
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetAddValidParamsTest,
    "PinWright.container.set.add.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetAddValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MySet"));
    Payload->SetStringField(TEXT("value"), TEXT("element"));
    TestTrue(TEXT("container.set.add invoked"), InvokeHandler(TEXT("container.set.add"), Payload));
    return true;
}

// ============================================================================
// container.set.remove
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetRemoveValidParamsTest,
    "PinWright.container.set.remove.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetRemoveValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MySet"));
    Payload->SetStringField(TEXT("value"), TEXT("element"));
    TestTrue(TEXT("container.set.remove invoked"), InvokeHandler(TEXT("container.set.remove"), Payload));
    return true;
}

// ============================================================================
// container.set.contains
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetContainsValidParamsTest,
    "PinWright.container.set.contains.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetContainsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MySet"));
    Payload->SetStringField(TEXT("value"), TEXT("element"));
    TestTrue(TEXT("container.set.contains invoked"), InvokeHandler(TEXT("container.set.contains"), Payload));
    return true;
}

// ============================================================================
// container.set.clear
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetClearValidParamsTest,
    "PinWright.container.set.clear.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetClearValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Game/NonExistentObject"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("MySet"));
    TestTrue(TEXT("container.set.clear invoked"), InvokeHandler(TEXT("container.set.clear"), Payload));
    return true;
}

// ============================================================================
// asset.references
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReferencesValidParamsTest,
    "PinWright.asset.references.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetReferencesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistentAsset"));
    TestTrue(TEXT("asset.references invoked"), InvokeHandler(TEXT("asset.references"), Payload));
    return true;
}

// ============================================================================
// asset.dependencies
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDependenciesValidParamsTest,
    "PinWright.asset.dependencies.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDependenciesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistentAsset"));
    TestTrue(TEXT("asset.dependencies invoked"), InvokeHandler(TEXT("asset.dependencies"), Payload));
    return true;
}

// ============================================================================
// property.list — default returns all reflected properties (B-property-list-hides-reflected-props)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyListDefaultReturnsAllReflectedTest,
    "PinWright.property.list.DefaultReturnsAllReflected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyListDefaultReturnsAllReflectedTest::RunTest(const FString& Parameters)
{
    UInstancedStaticMeshComponent* Component =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient instanced static mesh component created"), Component);
    if (!Component)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Component->GetPathName());
    // No editableOnly / includeAll — exercise the new default (all reflected).

    FTestResponseCapture Capture;
    TestTrue(
        TEXT("property.list handler found"),
        InvokeHandlerWithCapture(TEXT("property.list"), Payload, Capture));
    TestTrue(TEXT("property.list succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
    TestTrue(
        TEXT("property.list returned properties array"),
        Capture.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties);
    if (!Properties)
    {
        return false;
    }

    TestTrue(TEXT("default property.list returns > 0 entries"), Properties->Num() > 0);

    // Find at least one entry whose flags.edit is false OR flags.blueprintVisible is false —
    // i.e. a reflected property that the old filtered view would have hidden.
    bool bFoundNonEditable = false;
    for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
    {
        const TSharedPtr<FJsonObject> PropertyObject =
            PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
                ? PropertyValue->AsObject()
                : nullptr;
        if (!PropertyObject.IsValid())
        {
            continue;
        }

        const TSharedPtr<FJsonObject>* FlagsObject = nullptr;
        if (!PropertyObject->TryGetObjectField(TEXT("flags"), FlagsObject) ||
            !FlagsObject || !FlagsObject->IsValid())
        {
            continue;
        }

        bool bEdit = true;
        bool bBlueprintVisible = true;
        (*FlagsObject)->TryGetBoolField(TEXT("edit"), bEdit);
        (*FlagsObject)->TryGetBoolField(TEXT("blueprintVisible"), bBlueprintVisible);
        if (!bEdit || !bBlueprintVisible)
        {
            bFoundNonEditable = true;
            break;
        }
    }

    TestTrue(
        TEXT("default property.list surfaces at least one non-editable reflected property"),
        bFoundNonEditable);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyListEditableOnlyFiltersHiddenTest,
    "PinWright.property.list.EditableOnlyFiltersHidden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyListEditableOnlyFiltersHiddenTest::RunTest(const FString& Parameters)
{
    UInstancedStaticMeshComponent* Component =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient instanced static mesh component created"), Component);
    if (!Component)
    {
        return false;
    }

    // Default call (all reflected) for baseline count.
    TSharedPtr<FJsonObject> DefaultPayload = MakeShared<FJsonObject>();
    DefaultPayload->SetStringField(TEXT("objectPath"), Component->GetPathName());

    FTestResponseCapture DefaultCapture;
    TestTrue(
        TEXT("property.list default invocation found"),
        InvokeHandlerWithCapture(TEXT("property.list"), DefaultPayload, DefaultCapture));
    TestTrue(TEXT("property.list default succeeded"), DefaultCapture.bSuccess);
    if (!DefaultCapture.bSuccess || !DefaultCapture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* DefaultProperties = nullptr;
    DefaultCapture.Result->TryGetArrayField(TEXT("properties"), DefaultProperties);
    const int32 DefaultCount = DefaultProperties ? DefaultProperties->Num() : 0;

    // editableOnly:true should narrow the result.
    TSharedPtr<FJsonObject> FilteredPayload = MakeShared<FJsonObject>();
    FilteredPayload->SetStringField(TEXT("objectPath"), Component->GetPathName());
    FilteredPayload->SetBoolField(TEXT("editableOnly"), true);

    FTestResponseCapture FilteredCapture;
    TestTrue(
        TEXT("property.list editableOnly invocation found"),
        InvokeHandlerWithCapture(TEXT("property.list"), FilteredPayload, FilteredCapture));
    TestTrue(TEXT("property.list editableOnly succeeded"), FilteredCapture.bSuccess);
    if (!FilteredCapture.bSuccess || !FilteredCapture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* FilteredProperties = nullptr;
    TestTrue(
        TEXT("editableOnly returned properties array"),
        FilteredCapture.Result->TryGetArrayField(TEXT("properties"), FilteredProperties) &&
        FilteredProperties);
    if (!FilteredProperties)
    {
        return false;
    }

    TestTrue(
        TEXT("editableOnly count is strictly less than default count"),
        FilteredProperties->Num() < DefaultCount);

    // Every entry in the filtered view must be editable or blueprint-visible.
    for (const TSharedPtr<FJsonValue>& PropertyValue : *FilteredProperties)
    {
        const TSharedPtr<FJsonObject> PropertyObject =
            PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
                ? PropertyValue->AsObject()
                : nullptr;
        if (!PropertyObject.IsValid())
        {
            continue;
        }

        const TSharedPtr<FJsonObject>* FlagsObject = nullptr;
        const bool bHasFlags =
            PropertyObject->TryGetObjectField(TEXT("flags"), FlagsObject) &&
            FlagsObject && FlagsObject->IsValid();
        TestTrue(TEXT("editableOnly entry carries flags object"), bHasFlags);
        if (!bHasFlags)
        {
            continue;
        }

        bool bEdit = false;
        bool bBlueprintVisible = false;
        (*FlagsObject)->TryGetBoolField(TEXT("edit"), bEdit);
        (*FlagsObject)->TryGetBoolField(TEXT("blueprintVisible"), bBlueprintVisible);
        TestTrue(
            TEXT("editableOnly entry has edit||blueprintVisible"),
            bEdit || bBlueprintVisible);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyListEntriesCarryFlagsObjectTest,
    "PinWright.property.list.EntriesCarryFlagsObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyListEntriesCarryFlagsObjectTest::RunTest(const FString& Parameters)
{
    UInstancedStaticMeshComponent* Component =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient instanced static mesh component created"), Component);
    if (!Component)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Component->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(
        TEXT("property.list handler found"),
        InvokeHandlerWithCapture(TEXT("property.list"), Payload, Capture));
    TestTrue(TEXT("property.list succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
    TestTrue(
        TEXT("property.list returned properties array"),
        Capture.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties);
    if (!Properties || Properties->Num() == 0)
    {
        return false;
    }

    for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
    {
        const TSharedPtr<FJsonObject> PropertyObject =
            PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
                ? PropertyValue->AsObject()
                : nullptr;
        TestTrue(TEXT("property entry is an object"), PropertyObject.IsValid());
        if (!PropertyObject.IsValid())
        {
            continue;
        }

        const TSharedPtr<FJsonObject>* FlagsObject = nullptr;
        const bool bHasFlags =
            PropertyObject->TryGetObjectField(TEXT("flags"), FlagsObject) &&
            FlagsObject && FlagsObject->IsValid();
        TestTrue(TEXT("property entry has flags sub-object"), bHasFlags);
        if (!bHasFlags)
        {
            continue;
        }

        bool bDummy = false;
        TestTrue(TEXT("flags.edit present"), (*FlagsObject)->TryGetBoolField(TEXT("edit"), bDummy));
        TestTrue(TEXT("flags.blueprintVisible present"), (*FlagsObject)->TryGetBoolField(TEXT("blueprintVisible"), bDummy));
        TestTrue(TEXT("flags.editOnInstance present"), (*FlagsObject)->TryGetBoolField(TEXT("editOnInstance"), bDummy));
        TestTrue(TEXT("flags.transient present"), (*FlagsObject)->TryGetBoolField(TEXT("transient"), bDummy));
    }

    return true;
}
