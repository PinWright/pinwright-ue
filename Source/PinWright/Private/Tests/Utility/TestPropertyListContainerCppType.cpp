// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-property-list-container-cpptype-type-erased.
// property.list (and property.get, and system.inspect.inspect_class) report each
// UPROPERTY's cppType. The site is AddPropertyMetadataFields in
// Handlers/Utility/UtilityPropertyHandler.cpp (shared by property.list/property.get),
// which now routes through GetPropertyCppTypeWithParams (Utils/PropertyInspection.cpp).
// Counterfactual: if that helper is reverted to the bare no-arg Property->GetCPPType(),
// container properties emit the type-erased token "TMap"/"TArray"/"TSet" with no
// key/value/element type — these asserts then fail because the templated "<...>" form
// is gone. Driving the real property.list handler end-to-end (not the helper directly)
// proves the production discovery surface, not just the utility.
#include "TestPropertyListContainerCppType.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

#include "Tests/TestUtils.h"
#include "Tests/Utility/PropertyListTestHelpers.h"
#include "Utils/PropertyInspection.h"

namespace
{
    // Pull the cppType string for a named property out of a property.list result.
    FString CppTypeForProperty(const TSharedPtr<FJsonObject>& ListResult, const FString& PropertyName)
    {
        TSharedPtr<FJsonObject> Entry = PropertyListTestHelpers::FindPropertyEntry(ListResult, PropertyName);
        if (!Entry.IsValid())
        {
            return FString();
        }
        FString CppType;
        Entry->TryGetStringField(TEXT("cppType"), CppType);
        return CppType;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestPropertyListContainerCppType,
    "PinWright.Property.List.ContainerCppType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTestPropertyListContainerCppType::RunTest(const FString& Parameters)
{
    UTestPropertyListContainerCppTypeHost* Host =
        NewObject<UTestPropertyListContainerCppTypeHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host)
    {
        return false;
    }
    // Keep Host alive across the handler call; property resolution can trigger GC.
    Host->AddToRoot();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetBoolField(TEXT("includeValues"), false);
    Payload->SetBoolField(TEXT("includeDefault"), false);
    Payload->SetBoolField(TEXT("includeOverrideState"), false);
    Payload->SetBoolField(TEXT("includeMetadata"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("property.list"), Payload, Capture);
    TestTrue(TEXT("property.list handler found"), bFound);
    TestTrue(TEXT("property.list succeeded"), Capture.bSuccess);
    TestTrue(TEXT("property.list returned payload"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        Host->RemoveFromRoot();
        return false;
    }

    // Map: the bare token would be "TMap"; the fix emits the templated key/value form.
    const FString MapType = CppTypeForProperty(Capture.Result, TEXT("NameToString"));
    TestTrue(TEXT("map cppType starts with TMap"), MapType.StartsWith(TEXT("TMap")));
    TestTrue(TEXT("map cppType carries templated parameters (not bare 'TMap')"),
        MapType.Contains(TEXT("<")) && !MapType.Equals(TEXT("TMap")));
    TestTrue(TEXT("map cppType names its value type FString"), MapType.Contains(TEXT("FString")));

    // Array: bare token would be "TArray"; the fix emits "TArray<FVector>".
    const FString ArrayType = CppTypeForProperty(Capture.Result, TEXT("Points"));
    TestTrue(TEXT("array cppType starts with TArray"), ArrayType.StartsWith(TEXT("TArray")));
    TestTrue(TEXT("array cppType carries element type (not bare 'TArray')"),
        ArrayType.Contains(TEXT("<")) && !ArrayType.Equals(TEXT("TArray")));
    TestTrue(TEXT("array cppType names its element type FVector"), ArrayType.Contains(TEXT("FVector")));

    // Set: bare token would be "TSet"; the fix emits "TSet<FName>".
    const FString SetType = CppTypeForProperty(Capture.Result, TEXT("Tags"));
    TestTrue(TEXT("set cppType starts with TSet"), SetType.StartsWith(TEXT("TSet")));
    TestTrue(TEXT("set cppType carries element type (not bare 'TSet')"),
        SetType.Contains(TEXT("<")) && !SetType.Equals(TEXT("TSet")));

    // Scalar: non-container cppType is unchanged (the out-param stays empty -> no "<...>").
    const FString ScalarType = CppTypeForProperty(Capture.Result, TEXT("Scalar"));
    TestEqual(TEXT("scalar cppType is the plain type with no templated suffix"),
        ScalarType, FString(TEXT("float")));

    // Direct unit check on the shared helper that the handlers route through.
    if (FMapProperty* MapProp = CastField<FMapProperty>(
            UTestPropertyListContainerCppTypeHost::StaticClass()->FindPropertyByName(TEXT("NameToString"))))
    {
        const FString Direct = GetPropertyCppTypeWithParams(MapProp);
        TestTrue(TEXT("GetPropertyCppTypeWithParams emits templated map type"),
            Direct.StartsWith(TEXT("TMap<")) && Direct.Contains(TEXT("FString")));
    }

    Host->RemoveFromRoot();
    return true;
}
