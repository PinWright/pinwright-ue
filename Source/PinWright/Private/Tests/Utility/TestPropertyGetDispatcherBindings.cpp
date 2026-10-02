// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-python-bp-dispatcher-property-unsafe.
//
// python.execute's get_editor_property('<Dispatcher>').is_bound() answered False on a
// Blueprint event dispatcher that was bound, and dir() on the same value crashed the
// editor. Both are engine Python plugin behaviour; the typed answer is property.get,
// whose delegate marker reports bindingStatus and one {object, function} entry per bound
// invocation. This pins that contract on a real Blueprint dispatcher
// (FMulticastInlineDelegateProperty on a Blueprint-generated class), empty and bound.
//
// Counterfactual: if the multicast marker stops reading the invocation list (for example
// falls back to the plain {_kind, value} marker), bindingStatus and bindings[] disappear
// and the bound-state assertions fail.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
    TSharedPtr<FJsonObject> GetDispatcherValue(FAutomationTestBase& Test, UObject* Instance)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Instance->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), TEXT("OnFoo"));

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("property.get"), Payload, Capture) ||
            !Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("property.get failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Value = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("value"), Value) || !Value)
        {
            Test.AddError(TEXT("property.get value is not an object"));
            return nullptr;
        }
        return *Value;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyGetDispatcherBindingsTest,
    "PinWright.property.get.DispatcherBindingsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyGetDispatcherBindingsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/__PW_GatewayTests/BP_DispatcherRead_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UPackage* Package = CreatePackage(*AssetPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        UObject::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), AssetPath);
    AddPayload->SetStringField(TEXT("name"), TEXT("OnFoo"));
    FTestResponseCapture AddCapture;
    if (!InvokeHandlerWithCapture(TEXT("blueprint.add_dispatcher"), AddPayload, AddCapture) || !AddCapture.bSuccess)
    {
        AddError(FString::Printf(TEXT("blueprint.add_dispatcher failed: %s %s"), *AddCapture.ErrorCode, *AddCapture.Message));
        return false;
    }

    UClass* GeneratedClass = Blueprint->GeneratedClass.Get();
    FMulticastDelegateProperty* DispatcherProperty = GeneratedClass
        ? FindFProperty<FMulticastDelegateProperty>(GeneratedClass, TEXT("OnFoo"))
        : nullptr;
    if (!TestNotNull(TEXT("generated class has dispatcher OnFoo"), DispatcherProperty))
    {
        return false;
    }

    TStrongObjectPtr<UObject> Instance(NewObject<UObject>(GetTransientPackage(), GeneratedClass,
        *FString::Printf(TEXT("DispatcherReadProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));

    // Unbound: the read must say empty, not bound.
    if (const TSharedPtr<FJsonObject> Empty = GetDispatcherValue(*this, Instance.Get()))
    {
        TestEqual(TEXT("dispatcher reports the multicast-inline kind"),
            Empty->GetStringField(TEXT("_kind")), FString(TEXT("FMulticastInlineDelegateProperty")));
        TestEqual(TEXT("unbound dispatcher reads empty"),
            Empty->GetStringField(TEXT("bindingStatus")), FString(TEXT("empty")));
    }

    // Bind to a real UFUNCTION (IsBound requires the function to resolve on the object).
    UObject* Target = GetMutableDefault<UKismetSystemLibrary>();
    FScriptDelegate Binding;
    Binding.BindUFunction(Target, TEXT("PrintString"));
    DispatcherProperty->AddDelegate(Binding, Instance.Get());

    const TSharedPtr<FJsonObject> Bound = GetDispatcherValue(*this, Instance.Get());
    if (!Bound.IsValid())
    {
        return false;
    }
    TestEqual(TEXT("bound dispatcher reads bound"),
        Bound->GetStringField(TEXT("bindingStatus")), FString(TEXT("bound")));

    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    if (TestTrue(TEXT("bindings[] present"), Bound->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings) &&
        TestEqual(TEXT("one binding reported"), Bindings->Num(), 1))
    {
        const TSharedPtr<FJsonObject> Entry = (*Bindings)[0]->AsObject();
        TestEqual(TEXT("binding names the bound object"),
            Entry->GetStringField(TEXT("object")), Target->GetPathName());
        TestEqual(TEXT("binding names the bound function"),
            Entry->GetStringField(TEXT("function")), FString(TEXT("PrintString")));
    }
    return true;
}
