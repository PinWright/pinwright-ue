// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for PropertyUtils.cpp's instanced-subobject recursion.
// Before the fix, UPROPERTY(Instanced) UObject properties were dumped as bare
// subobject path strings; their actual configuration (ConfigInt, ConfigName, etc.)
// was invisible. The fix adds a CPF_PersistentInstance | CPF_InstancedReference
// check on explicitly UObject-owned property storage, routing owned refs through
// ExpandInstancedSubobject while explicitly raw sources remain path-shaped.
#include "TestAssetDumpInstancedSubobjects.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
    // Resolve PropName on Host's class, export it, and unwrap the result as a JSON
    // object. Returns null on any step's failure (missing property, invalid export,
    // or non-object JSON) so each test reduces to a single guard on the result.
    TSharedPtr<FJsonObject> ExportInstancedObjectField(UObject* Host, const TCHAR* PropName)
    {
        FProperty* Prop = Host->GetClass()->FindPropertyByName(PropName);
        if (!Prop) return nullptr;

        TSharedPtr<FJsonValue> Json = ExportPropertyToJsonValue(Host, Prop);
        if (!Json.IsValid()) return nullptr;

        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Json->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr)) return nullptr;
        return *ObjPtr;
    }

    // Resolve the array PropName on Host's class, export it, and unwrap element
    // Index as a JSON object. OutArrayNum receives the exported array length.
    TSharedPtr<FJsonObject> ExportInstancedArrayElement(
        UObject* Host, const TCHAR* PropName, int32 Index, int32& OutArrayNum)
    {
        OutArrayNum = INDEX_NONE;
        FProperty* Prop = Host->GetClass()->FindPropertyByName(PropName);
        if (!Prop) return nullptr;

        TSharedPtr<FJsonValue> Json = ExportPropertyToJsonValue(Host, Prop);
        if (!Json.IsValid()) return nullptr;

        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Json->TryGetArray(ArrayPtr) || !ArrayPtr) return nullptr;
        OutArrayNum = ArrayPtr->Num();
        if (!ArrayPtr->IsValidIndex(Index)) return nullptr;

        const TSharedPtr<FJsonObject>* ElemObjPtr = nullptr;
        if (!(*ArrayPtr)[Index]->TryGetObject(ElemObjPtr) || !ElemObjPtr || !(*ElemObjPtr)) return nullptr;
        return *ElemObjPtr;
    }

    // The same container exported from an explicitly raw source must not inherit
    // UObject ownership merely because its address happens to be a UObject.
    TSharedPtr<FJsonValue> ExportRawArrayElement(
        UObject* Host, const TCHAR* PropName, int32 Index, int32& OutArrayNum)
    {
        OutArrayNum = INDEX_NONE;
        FProperty* Prop = Host->GetClass()->FindPropertyByName(PropName);
        if (!Prop) return nullptr;

        TSharedPtr<FJsonValue> Json = ExportPropertyToJsonValue(
            FPropertyExportSource::FromRaw(Host), Prop);
        if (!Json.IsValid()) return nullptr;

        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Json->TryGetArray(ArrayPtr) || !ArrayPtr) return nullptr;
        OutArrayNum = ArrayPtr->Num();
        return ArrayPtr->IsValidIndex(Index) ? (*ArrayPtr)[Index] : nullptr;
    }
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInstancedSubobjectRecursionTest,
    "PinWright.utils.property_utils.InstancedSubobjectRecursion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInstancedSubobjectRecursionTest::RunTest(const FString& Parameters)
{
    UTestInstancedHost* Host = NewObject<UTestInstancedHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    Host->Payload = NewObject<UTestInstancedPayload>(Host);
    TestNotNull(TEXT("Payload subobject created"), Host->Payload.Get());
    if (!Host->Payload) return false;

    Host->Payload->ConfigInt = 42;
    Host->Payload->ConfigName = TEXT("alpha");

    UTestInstancedPayload* ArrayElem = NewObject<UTestInstancedPayload>(Host);
    TestNotNull(TEXT("Array element subobject created"), ArrayElem);
    if (!ArrayElem) return false;

    ArrayElem->ConfigInt = -7;
    ArrayElem->ConfigName = TEXT("beta");
    Host->PayloadArray.Add(ArrayElem);

    // Explicitly owned top-level and collection storage both expand. Counterfactual:
    // dropping owner provenance during array recursion turns PayloadArray[0] into a path.
    TSharedPtr<FJsonObject> PayloadObj = ExportInstancedObjectField(Host, TEXT("Payload"));
    TestNotNull(TEXT("Payload exported as EJson::Object"), PayloadObj.Get());
    if (!PayloadObj) return false;

    double ConfigIntVal = 0.0;
    TestTrue(TEXT("Payload.ConfigInt field present"),
        PayloadObj->TryGetNumberField(TEXT("ConfigInt"), ConfigIntVal));
    TestEqual(TEXT("Payload.ConfigInt == 42"), ConfigIntVal, 42.0);

    FString ConfigNameVal;
    TestTrue(TEXT("Payload.ConfigName field present"),
        PayloadObj->TryGetStringField(TEXT("ConfigName"), ConfigNameVal));
    TestEqual(TEXT("Payload.ConfigName == alpha"), ConfigNameVal, FString(TEXT("alpha")));

    // Array assertion: instanced array elements reached from the owner retain it.
    int32 ArrayNum = INDEX_NONE;
    TSharedPtr<FJsonObject> ElemObj = ExportInstancedArrayElement(Host, TEXT("PayloadArray"), 0, ArrayNum);
    TestEqual(TEXT("PayloadArray length == 1"), ArrayNum, 1);
    TestNotNull(TEXT("PayloadArray[0] is EJson::Object"), ElemObj.Get());
    if (!ElemObj) return false;

    double ElemConfigInt = 0.0;
    TestTrue(TEXT("PayloadArray[0].ConfigInt field present"),
        ElemObj->TryGetNumberField(TEXT("ConfigInt"), ElemConfigInt));
    TestEqual(TEXT("PayloadArray[0].ConfigInt == -7"), ElemConfigInt, -7.0);

    FString ElemConfigName;
    TestTrue(TEXT("PayloadArray[0].ConfigName field present"),
        ElemObj->TryGetStringField(TEXT("ConfigName"), ElemConfigName));
    TestEqual(TEXT("PayloadArray[0].ConfigName == beta"), ElemConfigName, FString(TEXT("beta")));

    int32 RawArrayNum = INDEX_NONE;
    const TSharedPtr<FJsonValue> RawElem =
        ExportRawArrayElement(Host, TEXT("PayloadArray"), 0, RawArrayNum);
    TestEqual(TEXT("Raw PayloadArray length == 1"), RawArrayNum, 1);
    TestTrue(TEXT("Raw PayloadArray[0] export returned a value"), RawElem.IsValid());
    if (!RawElem.IsValid()) return false;
    FString RawElemPath;
    TestTrue(TEXT("Raw PayloadArray[0] remains path-shaped"), RawElem->TryGetString(RawElemPath));
    TestEqual(TEXT("Raw PayloadArray[0] exports its object path"),
        RawElemPath, ArrayElem->GetPathName());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsSparseInstancedSubobjectDiffTest,
    "PinWright.utils.property_utils.SparseInstancedSubobjectDiff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsSparseInstancedSubobjectDiffTest::RunTest(const FString& Parameters)
{
    UTestInstancedHost* Host = NewObject<UTestInstancedHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    Host->Payload = NewObject<UTestInstancedPayload>(Host);
    TestNotNull(TEXT("Payload subobject created"), Host->Payload.Get());
    if (!Host->Payload) return false;

    Host->Payload->SparseEditableChanged = 314;
    Host->Payload->SparseHiddenChanged = 271;
    Host->Payload->SparseTransientChanged = 159;

    TSharedPtr<FJsonObject> PayloadObj = ExportInstancedObjectField(Host, TEXT("Payload"));
    TestNotNull(TEXT("Payload exported as EJson::Object"), PayloadObj.Get());
    if (!PayloadObj) return false;

    double SparseChanged = 0.0;
    TestTrue(TEXT("Changed editable field is included"),
        PayloadObj->TryGetNumberField(TEXT("SparseEditableChanged"), SparseChanged));
    TestEqual(TEXT("Changed editable field value"), SparseChanged, 314.0);

    TestFalse(TEXT("Unchanged editable field is omitted"),
        PayloadObj->HasField(TEXT("SparseEditableDefault")));
    TestFalse(TEXT("Changed non-edit field is omitted"),
        PayloadObj->HasField(TEXT("SparseHiddenChanged")));
    TestFalse(TEXT("Changed transient field is omitted"),
        PayloadObj->HasField(TEXT("SparseTransientChanged")));

    UInstancedStaticMeshComponent* Component =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Instanced static mesh component created"), Component);
    if (!Component) return false;

    Component->AddInstance(FTransform::Identity);

    TSharedPtr<FJsonObject> Diff = BuildSparsePropertyDiffJson(
        Component,
        UInstancedStaticMeshComponent::StaticClass()->GetDefaultObject());
    TestTrue(TEXT("Sparse diff object is valid"), Diff.IsValid());
    if (!Diff.IsValid()) return false;

    const TSharedPtr<FJsonObject>* PlaceholderObj = nullptr;
    const bool bGotObject = Diff->TryGetObjectField(TEXT("PerInstanceSMData"), PlaceholderObj);
    TestTrue(TEXT("PerInstanceSMData emits an oversized placeholder"),
        bGotObject && PlaceholderObj != nullptr && PlaceholderObj->IsValid());
    if (!bGotObject || !PlaceholderObj || !PlaceholderObj->IsValid()) return false;

    FString ReasonStr;
    const bool bGotReason = (*PlaceholderObj)->TryGetStringField(TEXT("$reason"), ReasonStr);
    TestTrue(TEXT("PerInstanceSMData placeholder reason is exceeds-llm-budget"),
        bGotReason && ReasonStr == TEXT("exceeds-llm-budget"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsInstancedSubobjectClassDiscriminatorTest,
    "PinWright.utils.property_utils.InstancedSubobjectClassDiscriminator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsInstancedSubobjectClassDiscriminatorTest::RunTest(const FString& Parameters)
{
    // A polymorphic Instanced UObject element must carry a `_kind` class
    // discriminator so a defaults-only (CDO-identical) element is identifiable
    // instead of collapsing to a bare `{}`. Counterfactual: if the
    // Obj->SetStringField(TEXT("_kind"), ...) tag in ExpandInstancedSubobject is
    // reverted, the defaults-only element below serializes as `{}` and the
    // _kind assertions fail.
    UTestInstancedHost* Host = NewObject<UTestInstancedHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    // Top-level instanced subobject with a changed field: still gets _kind.
    Host->Payload = NewObject<UTestInstancedPayload>(Host);
    TestNotNull(TEXT("Payload subobject created"), Host->Payload.Get());
    if (!Host->Payload) return false;
    Host->Payload->ConfigInt = 5;

    // Array element left entirely at class defaults: the regression case that
    // previously erased the class and collapsed to `{}`.
    UTestInstancedPayload* DefaultsElem = NewObject<UTestInstancedPayload>(Host);
    TestNotNull(TEXT("Defaults-only array element created"), DefaultsElem);
    if (!DefaultsElem) return false;
    Host->PayloadArray.Add(DefaultsElem);

    const FString ExpectedKind = UTestInstancedPayload::StaticClass()->GetPathName();

    TSharedPtr<FJsonObject> PayloadObj = ExportInstancedObjectField(Host, TEXT("Payload"));
    TestNotNull(TEXT("Payload exported as EJson::Object"), PayloadObj.Get());
    if (!PayloadObj) return false;

    FString PayloadKind;
    TestTrue(TEXT("Payload carries _kind class discriminator"),
        PayloadObj->TryGetStringField(TEXT("_kind"), PayloadKind));
    TestEqual(TEXT("Payload _kind is the subobject class path"), PayloadKind, ExpectedKind);

    int32 ArrayNum = INDEX_NONE;
    TSharedPtr<FJsonObject> ElemObj = ExportInstancedArrayElement(Host, TEXT("PayloadArray"), 0, ArrayNum);
    TestEqual(TEXT("PayloadArray length == 1"), ArrayNum, 1);
    TestNotNull(TEXT("PayloadArray[0] is EJson::Object"), ElemObj.Get());
    if (!ElemObj) return false;

    FString ElemKind;
    TestTrue(TEXT("Defaults-only element carries _kind class discriminator"),
        ElemObj->TryGetStringField(TEXT("_kind"), ElemKind));
    TestEqual(TEXT("Defaults-only element _kind is the subobject class path"), ElemKind, ExpectedKind);
    TestTrue(TEXT("Defaults-only element is no longer an empty object"),
        ElemObj->Values.Num() >= 1);

    int32 RawArrayNum = INDEX_NONE;
    const TSharedPtr<FJsonValue> RawElem =
        ExportRawArrayElement(Host, TEXT("PayloadArray"), 0, RawArrayNum);
    TestEqual(TEXT("Raw defaults-only array length == 1"), RawArrayNum, 1);
    TestTrue(TEXT("Raw defaults-only element export returned a value"), RawElem.IsValid());
    if (!RawElem.IsValid()) return false;
    FString RawElemPath;
    TestTrue(TEXT("Raw defaults-only element remains path-shaped"), RawElem->TryGetString(RawElemPath));
    TestEqual(TEXT("Raw defaults-only element path"), RawElemPath, DefaultsElem->GetPathName());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpOwnedComponentPointerRecursionTest,
    "PinWright.utils.property_utils.OwnedComponentPointerRecursion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpOwnedComponentPointerRecursionTest::RunTest(const FString& Parameters)
{
    UTestInstancedHost* Host = NewObject<UTestInstancedHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    Host->OwnedComponentPointer = NewObject<USceneComponent>(Host, TEXT("OwnedComponentPointer"));
    TestNotNull(TEXT("Owned component created"), Host->OwnedComponentPointer.Get());
    if (!Host->OwnedComponentPointer) return false;
    Host->OwnedComponentPointer->ComponentTags.Add(TEXT("OwnedTag"));

    Host->ExternalComponentPointer = NewObject<USceneComponent>(GetTransientPackage(), TEXT("ExternalComponentPointer"));
    TestNotNull(TEXT("External component created"), Host->ExternalComponentPointer.Get());
    if (!Host->ExternalComponentPointer) return false;

    FProperty* OwnedProp = UTestInstancedHost::StaticClass()->FindPropertyByName(TEXT("OwnedComponentPointer"));
    TestNotNull(TEXT("OwnedComponentPointer FProperty resolved"), OwnedProp);
    if (!OwnedProp) return false;

    FProperty* ExternalProp = UTestInstancedHost::StaticClass()->FindPropertyByName(TEXT("ExternalComponentPointer"));
    TestNotNull(TEXT("ExternalComponentPointer FProperty resolved"), ExternalProp);
    if (!ExternalProp) return false;

    TSharedPtr<FJsonValue> OwnedJson = ExportPropertyToJsonValue(Host, OwnedProp);
    TestTrue(TEXT("Owned component export returned a value"), OwnedJson.IsValid());
    if (!OwnedJson.IsValid()) return false;

    const TSharedPtr<FJsonObject>* OwnedObjPtr = nullptr;
    TestTrue(TEXT("Owned component pointer exports as object"), OwnedJson->TryGetObject(OwnedObjPtr));
    if (!OwnedObjPtr || !(*OwnedObjPtr)) return false;

    const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
    TestTrue(TEXT("Owned component includes ComponentTags"),
        (*OwnedObjPtr)->TryGetArrayField(TEXT("ComponentTags"), Tags));
    if (!Tags || Tags->Num() < 1) return false;

    FString TagValue;
    TestTrue(TEXT("Owned component tag is a string"), (*Tags)[0]->TryGetString(TagValue));
    TestEqual(TEXT("Owned component tag exported"), TagValue, FString(TEXT("OwnedTag")));

    TSharedPtr<FJsonValue> ExternalJson = ExportPropertyToJsonValue(Host, ExternalProp);
    TestTrue(TEXT("External component export returned a value"), ExternalJson.IsValid());
    if (!ExternalJson.IsValid()) return false;

    FString ExternalPath;
    TestTrue(TEXT("External component pointer stays path-shaped"), ExternalJson->TryGetString(ExternalPath));
    TestEqual(TEXT("External component path exported"),
        ExternalPath,
        Host->ExternalComponentPointer->GetPathName());

    return true;
}

// COUNTERFACTUAL: if property.get forwards its resolved UObject container as raw
// void* storage, the owned component below is reduced to a path string and the
// ComponentTags assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPropertyGetOwnedComponentSerializationTest,
    "PinWright.property.get.OwnedComponentSerialization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPropertyGetOwnedComponentSerializationTest::RunTest(const FString& Parameters)
{
    UTestInstancedHost* Host = NewObject<UTestInstancedHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;
    const TStrongObjectPtr<UTestInstancedHost> HostGuard(Host);

    Host->OwnedComponentPointer = NewObject<USceneComponent>(Host, TEXT("OwnedComponentPointer"));
    TestNotNull(TEXT("Owned component created"), Host->OwnedComponentPointer.Get());
    if (!Host->OwnedComponentPointer) return false;
    Host->OwnedComponentPointer->ComponentTags.Add(TEXT("OwnedTag"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("OwnedComponentPointer"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("property.get handler found"),
        InvokeHandlerWithCapture(TEXT("property.get"), Payload, Capture));
    TestTrue(TEXT("property.get succeeded"), Capture.bSuccess);
    TestTrue(TEXT("property.get returned a payload"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return false;

    const TSharedPtr<FJsonObject>* ValueObject = nullptr;
    TestTrue(TEXT("property.get value preserves owned component expansion"),
        Capture.Result->TryGetObjectField(TEXT("value"), ValueObject));
    if (!ValueObject || !ValueObject->IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
    TestTrue(TEXT("property.get owned component includes ComponentTags"),
        (*ValueObject)->TryGetArrayField(TEXT("ComponentTags"), Tags));
    if (!Tags || Tags->Num() < 1) return false;

    FString TagValue;
    TestTrue(TEXT("property.get owned component tag is a string"),
        (*Tags)[0]->TryGetString(TagValue));
    TestEqual(TEXT("property.get owned component tag exported"),
        TagValue, FString(TEXT("OwnedTag")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsEmptyDelegateSerializationTest,
    "PinWright.utils.property_utils.EmptyDelegateSerialization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsEmptyDelegateSerializationTest::RunTest(const FString& Parameters)
{
    UTestPropertyUtilsDelegateHost* Host = NewObject<UTestPropertyUtilsDelegateHost>(GetTransientPackage());
    TestNotNull(TEXT("Delegate host created"), Host);
    if (!Host) return false;

    FProperty* InlineProp = UTestPropertyUtilsDelegateHost::StaticClass()->FindPropertyByName(TEXT("OnInlineEvent"));
    TestNotNull(TEXT("OnInlineEvent FProperty resolved"), InlineProp);
    if (!InlineProp) return false;

    TSharedPtr<FJsonValue> InlineJson = ExportPropertyToJsonValue(Host, InlineProp);
    TestTrue(TEXT("Inline delegate export returned a value"), InlineJson.IsValid());
    if (!InlineJson.IsValid()) return false;

    const TSharedPtr<FJsonObject>* InlineObjPtr = nullptr;
    TestTrue(TEXT("Inline delegate exports as object"), InlineJson->TryGetObject(InlineObjPtr));
    if (!InlineObjPtr || !(*InlineObjPtr)) return false;

    FString InlineKind;
    TestTrue(TEXT("Inline delegate has _kind"), (*InlineObjPtr)->TryGetStringField(TEXT("_kind"), InlineKind));
    TestEqual(TEXT("Inline delegate kind"), InlineKind, FString(TEXT("FMulticastInlineDelegateProperty")));
    TestFalse(TEXT("Inline delegate omits raw value field"), (*InlineObjPtr)->HasField(TEXT("value")));

    const TArray<TSharedPtr<FJsonValue>>* InlineBindings = nullptr;
    TestTrue(TEXT("Inline delegate has bindings array"),
        (*InlineObjPtr)->TryGetArrayField(TEXT("bindings"), InlineBindings));
    if (!InlineBindings) return false;
    TestEqual(TEXT("Inline delegate bindings empty"), InlineBindings->Num(), 0);

    USceneComponent* Component = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Sparse delegate component created"), Component);
    if (!Component) return false;

    FProperty* SparseProp = UActorComponent::StaticClass()->FindPropertyByName(TEXT("OnComponentActivated"));
    TestNotNull(TEXT("OnComponentActivated FProperty resolved"), SparseProp);
    if (!SparseProp) return false;

    TSharedPtr<FJsonValue> SparseJson = ExportPropertyToJsonValue(Component, SparseProp);
    TestTrue(TEXT("Sparse delegate export returned a value"), SparseJson.IsValid());
    if (!SparseJson.IsValid()) return false;

    const TSharedPtr<FJsonObject>* SparseObjPtr = nullptr;
    TestTrue(TEXT("Sparse delegate exports as object"), SparseJson->TryGetObject(SparseObjPtr));
    if (!SparseObjPtr || !(*SparseObjPtr)) return false;

    FString SparseKind;
    TestTrue(TEXT("Sparse delegate has _kind"), (*SparseObjPtr)->TryGetStringField(TEXT("_kind"), SparseKind));
    TestEqual(TEXT("Sparse delegate kind"), SparseKind, FString(TEXT("FMulticastSparseDelegateProperty")));
    TestFalse(TEXT("Sparse delegate omits raw value field"), (*SparseObjPtr)->HasField(TEXT("value")));

    const TArray<TSharedPtr<FJsonValue>>* SparseBindings = nullptr;
    TestTrue(TEXT("Sparse delegate has bindings array"),
        (*SparseObjPtr)->TryGetArrayField(TEXT("bindings"), SparseBindings));
    if (!SparseBindings) return false;
    TestEqual(TEXT("Sparse delegate bindings empty"), SparseBindings->Num(), 0);

    return true;
}

// COUNTERFACTUAL: reverting the raw-container fix reinterprets the struct bytes
// as a UObject and crashes before either component-value assertion completes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPropertyExportStructComponentContainerTest,
    "PinWright.utils.property_utils.StructComponentContainer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPropertyExportStructComponentContainerTest::RunTest(const FString& Parameters)
{
    USceneComponent* Component = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Component created"), Component);
    if (!Component) return false;
    const TStrongObjectPtr<USceneComponent> ComponentGuard(Component);

    FTestPropertyExportComponentContainer Container;
    Container.PopulatedComponent = Component;

    FProperty* PopulatedProperty =
        FTestPropertyExportComponentContainer::StaticStruct()->FindPropertyByName(TEXT("PopulatedComponent"));
    FProperty* NullProperty =
        FTestPropertyExportComponentContainer::StaticStruct()->FindPropertyByName(TEXT("NullComponent"));
    TestNotNull(TEXT("PopulatedComponent property found"), PopulatedProperty);
    TestNotNull(TEXT("NullComponent property found"), NullProperty);
    if (!PopulatedProperty || !NullProperty) return false;

    TSharedPtr<FJsonValue> PopulatedJson =
        ExportPropertyToJsonValue(&Container, PopulatedProperty);
    TestTrue(TEXT("Populated component export returned a value"), PopulatedJson.IsValid());
    if (!PopulatedJson.IsValid()) return false;

    FString ComponentPath;
    TestTrue(TEXT("Populated component exports as a string"), PopulatedJson->TryGetString(ComponentPath));
    TestEqual(TEXT("Populated component exports its object path"), ComponentPath, Component->GetPathName());

    TSharedPtr<FJsonValue> NullJson = ExportPropertyToJsonValue(&Container, NullProperty);
    TestTrue(TEXT("Null component export returned a value"), NullJson.IsValid());
    if (!NullJson.IsValid()) return false;
    TestEqual(TEXT("Null component exports as JSON null"), NullJson->Type, EJson::Null);
    return true;
}

// COUNTERFACTUAL: scanning emitted JSON for the legacy unsupported marker rejects
// this valid user map solely because its keys and value resemble that marker.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPropertyExportStrictMarkerShapedMapTest,
    "PinWright.utils.property_utils.StrictMarkerShapedMap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPropertyExportStrictMarkerShapedMapTest::RunTest(const FString& Parameters)
{
    FTestPropertyExportComponentContainer Container;
    Container.MarkerShapedMap.Add(TEXT("_kind"), TEXT("unsupported"));
    Container.MarkerShapedMap.Add(TEXT("cpp_type"), TEXT("legitimate-data"));

    FProperty* Property =
        FTestPropertyExportComponentContainer::StaticStruct()->FindPropertyByName(TEXT("MarkerShapedMap"));
    TestNotNull(TEXT("MarkerShapedMap property found"), Property);
    if (!Property) return false;

    TestFalse(TEXT("nullptr selects the raw source without overload ambiguity"),
        ExportPropertyToJsonValue(nullptr, Property).IsValid());

    const FPropertyExportResult Export = ExportPropertyToJsonValueStrict(
        FPropertyExportSource::FromRaw(&Container), Property);
    TestTrue(TEXT("Marker-shaped valid map remains supported"), Export.bSupported);
    TestTrue(TEXT("Marker-shaped valid map returns a value"), Export.Value.IsValid());
    if (!Export.bSupported || !Export.Value.IsValid()) return false;

    const TSharedPtr<FJsonObject>* MapObject = nullptr;
    TestTrue(TEXT("Marker-shaped map exports as an object"),
        Export.Value->TryGetObject(MapObject));
    if (!MapObject || !MapObject->IsValid()) return false;

    FString Kind;
    FString CppType;
    TestTrue(TEXT("Marker-shaped _kind entry is preserved"),
        (*MapObject)->TryGetStringField(TEXT("_kind"), Kind));
    TestTrue(TEXT("Marker-shaped cpp_type entry is preserved"),
        (*MapObject)->TryGetStringField(TEXT("cpp_type"), CppType));
    TestEqual(TEXT("Marker-shaped _kind entry value"), Kind, FString(TEXT("unsupported")));
    TestEqual(TEXT("Marker-shaped cpp_type entry value"), CppType, FString(TEXT("legitimate-data")));
    return true;
}

// COUNTERFACTUAL: if an array or struct recursion drops its child's unsupported
// status, strict export returns a partial value (or a legacy marker nested inside
// it) instead of rejecting the complete response shape. The loss-tolerant dump path
// takes the opposite contract: it must mark the unsupported member in place and keep
// every sibling it can decompose — collapsing the whole element to one marker erased
// the AssetPath of every FSoftObjectPath in the dump mirror.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPropertyExportStrictNestedUnsupportedTest,
    "PinWright.utils.property_utils.StrictNestedUnsupported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPropertyExportStrictNestedUnsupportedTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    FTestPropertyExportComponentContainer Container;
    Container.UnsupportedNestedValues.Add(FSoftObjectPath::ConstructFromStringPath(
        FString(TEXT("/Game/UnsupportedNested.Asset:Subobject"))));

    FProperty* Property =
        FTestPropertyExportComponentContainer::StaticStruct()->FindPropertyByName(
            TEXT("UnsupportedNestedValues"));
    TestNotNull(TEXT("UnsupportedNestedValues property found"), Property);
    if (!Property) return false;

    const FPropertyExportSource Source = FPropertyExportSource::FromRaw(&Container);
    const FPropertyExportResult StrictExport = ExportPropertyToJsonValueStrict(Source, Property);
    TestFalse(TEXT("Unsupported nested value rejects strict export"), StrictExport.bSupported);
    TestFalse(TEXT("Unsupported strict export has no partial value"), StrictExport.Value.IsValid());

    const TSharedPtr<FJsonValue> LegacyExport = ExportPropertyToJsonValue(Source, Property);
    TestTrue(TEXT("Loss-tolerant export synthesizes a value"), LegacyExport.IsValid());
    if (!LegacyExport.IsValid()) return false;

    // The array is expanded, not replaced: the one element is an object carrying the
    // decomposed AssetPath alongside the marker for the member that could not be read.
    const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
    TestTrue(TEXT("Loss-tolerant export keeps the array shape"),
        LegacyExport->TryGetArray(Elements));
    if (!Elements || Elements->Num() != 1)
    {
        AddError(TEXT("Loss-tolerant export did not emit exactly one array element"));
        return false;
    }

    const TSharedPtr<FJsonObject>* ElementObject = nullptr;
    TestTrue(TEXT("Loss-tolerant element is an expanded object"),
        (*Elements)[0]->TryGetObject(ElementObject));
    if (!ElementObject || !ElementObject->IsValid()) return false;

    const TSharedPtr<FJsonObject>* AssetPathObject = nullptr;
    TestTrue(TEXT("Expanded element keeps the supported AssetPath member"),
        (*ElementObject)->TryGetObjectField(TEXT("AssetPath"), AssetPathObject));
    if (AssetPathObject && AssetPathObject->IsValid())
    {
        FString PackageName;
        (*AssetPathObject)->TryGetStringField(TEXT("PackageName"), PackageName);
        TestEqual(TEXT("Expanded element carries the real package name"),
            PackageName, FString(TEXT("/Game/UnsupportedNested")));
    }

    const TSharedPtr<FJsonObject>* MarkerObject = nullptr;
    TestTrue(TEXT("Unsupported member carries the marker in place"),
        (*ElementObject)->TryGetObjectField(TEXT("SubPathString"), MarkerObject));
    if (!MarkerObject || !MarkerObject->IsValid()) return false;

    FString Kind;
    TestTrue(TEXT("Loss-tolerant marker has _kind"),
        (*MarkerObject)->TryGetStringField(TEXT("_kind"), Kind));
    TestEqual(TEXT("Loss-tolerant marker identifies unsupported"),
        Kind, FString(TEXT("unsupported")));
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-has-no-utf8-child"),
        TEXT("FSoftObjectPath exposes its unsupported FUtf8String child on UE 5.7+."));
#endif
    return true;
}
