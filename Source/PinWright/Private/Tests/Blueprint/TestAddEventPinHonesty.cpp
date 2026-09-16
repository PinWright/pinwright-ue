// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.add_event used to hardcode success:true and echo the caller's own `parameters`
// array back, while pins that failed to be created went to a UE_LOG and nowhere else. The same
// request array was written into the blueprint registry, and blueprint.get merged it back into
// its response — so the write path and the readback corroborated each other while both
// disagreed with the graph, and the agent's next call (connect_pins) targeted pins that do not
// exist.
//
// These tests drive the production handlers and assert the FAILURE direction of each half:
//   - a parameter type that would silently become a wildcard pin is refused before anything is
//     created (TYPE_NOT_FOUND, no node in the graph);
//   - `parameters` on a built-in event, where they are structurally impossible, is refused
//     (UNSUPPORTED_ARGUMENT, no node added) rather than dropped and echoed;
//   - a pin the node refuses makes the verb report failure, and the response still names the
//     pins that DO exist (PIN_CREATION_FAILED);
//   - blueprint.get does not report an event that only the registry believes in.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// Named (not anonymous) namespace: TestBlueprintHandlers.cpp already defines
// MakeUniqueAssetPath / CleanupAsset in an anonymous namespace, and a Unity blob merges these
// translation units. Distinct names in a named namespace keep both definitions legal.
namespace AddEventPinHonestyTestUtils
{
    inline FString MakeAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Detach rather than force-delete: UEditorAssetLibrary::DeleteAsset routes through
    // ObjectTools::ForceDeleteObjects, whose referencer sweep walks every live UObject.
    // CleanupTestAsset takes the package-path form directly and derives the object path itself.
    inline void DeleteAssetIfPresent(const FString& PackagePath)
    {
        if (PackagePath.IsEmpty())
        {
            return;
        }
        CleanupTestAsset(PackagePath);
    }

    inline UBlueprint* MakeActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    inline UK2Node_CustomEvent* FindCustomEvent(UBlueprint* Blueprint, const FName EventName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
                if (CustomEvent && CustomEvent->CustomFunctionName == EventName)
                {
                    return CustomEvent;
                }
            }
        }
        return nullptr;
    }

    inline int32 CountEventNodes(UBlueprint* Blueprint)
    {
        int32 Count = 0;
        if (!Blueprint)
        {
            return Count;
        }
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Cast<UK2Node_Event>(Node) != nullptr)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    // Returns the "name" of the entry at Index of the named object array, or an empty string.
    inline FString ArrayEntryStringField(const TSharedPtr<FJsonObject>& Object,
        const TCHAR* ArrayName, int32 Index, const TCHAR* Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(ArrayName, Arr) || !Arr
            || !Arr->IsValidIndex(Index) || !(*Arr)[Index].IsValid())
        {
            return FString();
        }
        const TSharedPtr<FJsonObject> Entry = (*Arr)[Index]->AsObject();
        FString Out;
        if (Entry.IsValid())
        {
            Entry->TryGetStringField(Field, Out);
        }
        return Out;
    }
}

// ============================================================================
// FAILURE DIRECTION: an unresolvable parameter type is refused, and nothing is created.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEventRejectsWildcardParamTest,
    "PinWright.blueprint.add_event.UnresolvableParamTypeIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAddEventRejectsWildcardParamTest::RunTest(const FString& Parameters)
{
    using namespace AddEventPinHonestyTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("AddEventWildcardParam"));
    UBlueprint* BP = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        DeleteAssetIfPresent(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventType"), TEXT("custom"));
    Payload->SetStringField(TEXT("customEventName"), TEXT("PwWildcardEvent"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    TArray<TSharedPtr<FJsonValue>> ParamsArray;
    TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
    ParamObj->SetStringField(TEXT("name"), TEXT("Doomed"));
    // A bare identifier that names no UClass / UEnum / UScriptStruct. AddUserDefinedPin would
    // have made it a PC_Wildcard pin and still returned true, so it was not even counted as a
    // failure — the caller got success plus its own type string echoed back.
    ParamObj->SetStringField(TEXT("type"), TEXT("PwNoSuchTypeIdentifier"));
    ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    Payload->SetArrayField(TEXT("parameters"), ParamsArray);

    FTestResponseCapture Capture;
    if (TestTrue(TEXT("blueprint.add_event handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_event"), Payload, Capture)))
    {
        TestFalse(TEXT("add_event refuses a parameter type it cannot resolve"), Capture.bSuccess);
        TestEqual(TEXT("refusal uses TYPE_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("TYPE_NOT_FOUND")));
        // Validate-before-mutate: the refusal must leave no event behind.
        TestNull(TEXT("no event node was created by the refused call"),
            FindCustomEvent(BP, FName(TEXT("PwWildcardEvent"))));
    }

    DeleteAssetIfPresent(AssetPath);
    return true;
}

// ============================================================================
// FAILURE DIRECTION: `parameters` on a built-in event is refused, not dropped and echoed.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEventRejectsParamsOnBuiltinTest,
    "PinWright.blueprint.add_event.ParametersOnBuiltinEventAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAddEventRejectsParamsOnBuiltinTest::RunTest(const FString& Parameters)
{
    using namespace AddEventPinHonestyTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("AddEventBuiltinParams"));
    UBlueprint* BP = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        DeleteAssetIfPresent(AssetPath);
        return true;
    }
    const int32 EventNodesBefore = CountEventNodes(BP);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventType"), TEXT("BeginPlay"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    TArray<TSharedPtr<FJsonValue>> ParamsArray;
    TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
    ParamObj->SetStringField(TEXT("name"), TEXT("Ignored"));
    ParamObj->SetStringField(TEXT("type"), TEXT("bool"));
    ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    Payload->SetArrayField(TEXT("parameters"), ParamsArray);

    FTestResponseCapture Capture;
    if (TestTrue(TEXT("blueprint.add_event handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_event"), Payload, Capture)))
    {
        // A built-in event node takes its signature from the overridden UFunction; these pins
        // were silently dropped from the graph and then echoed back in the response.
        TestFalse(TEXT("add_event refuses parameters on a built-in event"), Capture.bSuccess);
        TestEqual(TEXT("refusal uses UNSUPPORTED_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ARGUMENT")));
        TestEqual(TEXT("no event node was added by the refused call"),
            CountEventNodes(BP), EventNodesBefore);
    }

    DeleteAssetIfPresent(AssetPath);
    return true;
}

// ============================================================================
// FAILURE DIRECTION: a parameter with no name is refused, and the message blames the name
// rather than the type.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEventRejectsUnnamedParamTest,
    "PinWright.blueprint.add_event.EmptyParameterNameIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAddEventRejectsUnnamedParamTest::RunTest(const FString& Parameters)
{
    using namespace AddEventPinHonestyTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("AddEventUnnamedParam"));
    UBlueprint* BP = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        DeleteAssetIfPresent(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventType"), TEXT("custom"));
    Payload->SetStringField(TEXT("customEventName"), TEXT("PwUnnamedParamEvent"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    TArray<TSharedPtr<FJsonValue>> ParamsArray;
    TSharedPtr<FJsonObject> BadParam = MakeShared<FJsonObject>();
    // AddUserDefinedPin rejects an empty name outright (BlueprintHandlerUtils.cpp:
    // `if (CleanName.IsEmpty()) return false;`). Before the fix that landed in a block-local
    // FailedParams, went to a UE_LOG, and the call still answered success:true with the
    // caller's own parameters array echoed back.
    BadParam->SetStringField(TEXT("name"), TEXT(""));
    BadParam->SetStringField(TEXT("type"), TEXT("bool"));
    ParamsArray.Add(MakeShared<FJsonValueObject>(BadParam));
    Payload->SetArrayField(TEXT("parameters"), ParamsArray);

    FTestResponseCapture Capture;
    if (TestTrue(TEXT("blueprint.add_event handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_event"), Payload, Capture)))
    {
        TestFalse(TEXT("add_event refuses a parameter with no name"), Capture.bSuccess);
        TestEqual(TEXT("refusal uses INVALID_ARGUMENT, not a type error"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestNull(TEXT("no event node was created by the refused call"),
            FindCustomEvent(BP, FName(TEXT("PwUnnamedParamEvent"))));
    }

    DeleteAssetIfPresent(AssetPath);
    return true;
}

// ============================================================================
// The response's parameters[] is READ OFF the node, not echoed from the request.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEventParametersAreMeasuredTest,
    "PinWright.blueprint.add_event.ParametersAreMeasuredNotEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAddEventParametersAreMeasuredTest::RunTest(const FString& Parameters)
{
    using namespace AddEventPinHonestyTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("AddEventMeasuredParams"));
    UBlueprint* BP = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        DeleteAssetIfPresent(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventType"), TEXT("custom"));
    Payload->SetStringField(TEXT("customEventName"), TEXT("PwMeasuredEvent"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    TArray<TSharedPtr<FJsonValue>> ParamsArray;
    TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
    // Padded on purpose: AddUserDefinedPin trims before creating the pin, so the pin the node
    // ends up with is named "Good". A response that reports "  Good  " is echoing the request;
    // a response that reports "Good" has read the node. This is the whole fix in one assertion.
    ParamObj->SetStringField(TEXT("name"), TEXT("  Good  "));
    ParamObj->SetStringField(TEXT("type"), TEXT("bool"));
    ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    Payload->SetArrayField(TEXT("parameters"), ParamsArray);

    FTestResponseCapture Capture;
    if (TestTrue(TEXT("blueprint.add_event handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_event"), Payload, Capture)))
    {
        TestTrue(TEXT("blueprint.add_event succeeded"), Capture.bSuccess);
        TestEqual(TEXT("parameters[] reports the pin the node really has, not the request"),
            ArrayEntryStringField(Capture.Result, TEXT("parameters"), 0, TEXT("name")),
            FString(TEXT("Good")));

        // Graph truth: the trimmed pin is the one that exists.
        UK2Node_CustomEvent* Node = FindCustomEvent(BP, FName(TEXT("PwMeasuredEvent")));
        if (TestNotNull(TEXT("the custom event node exists"), Node))
        {
            TestNotNull(TEXT("the trimmed pin was created"), Node->FindPin(TEXT("Good")));
        }
    }

    DeleteAssetIfPresent(AssetPath);
    return true;
}

// ============================================================================
// The readback must be an independent witness: blueprint.get reports the graph, so an event the
// registry still remembers but the graph no longer has must NOT appear.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGetDropsPhantomRegistryEventTest,
    "PinWright.blueprint.get.RegistryOnlyEventIsNotReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintGetDropsPhantomRegistryEventTest::RunTest(const FString& Parameters)
{
    using namespace AddEventPinHonestyTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("BlueprintGetPhantomEvent"));
    UBlueprint* BP = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        DeleteAssetIfPresent(AssetPath);
        return true;
    }

    const FName EventName(TEXT("PwPhantomEvent"));

    // 1. Author the event through the production verb, which records it in the registry.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), AssetPath);
        Payload->SetStringField(TEXT("eventType"), TEXT("custom"));
        Payload->SetStringField(TEXT("customEventName"), EventName.ToString());
        Payload->SetNumberField(TEXT("x"), 0.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("blueprint.add_event handler found"),
                InvokeHandlerWithCapture(TEXT("blueprint.add_event"), Payload, Capture)))
        {
            DeleteAssetIfPresent(AssetPath);
            return true;
        }
        if (!TestTrue(TEXT("blueprint.add_event succeeded"), Capture.bSuccess))
        {
            DeleteAssetIfPresent(AssetPath);
            return true;
        }
    }

    // 2. Delete the node behind the registry's back — the shape a default-mode compile_bpir
    //    Phase 0 sweep produces (docs/wiki-src/blueprint.bpir-gotchas.md). Deliberately NOT
    //    blueprint.remove_event, which would also clear the registry record and prove nothing.
    UK2Node_CustomEvent* Node = FindCustomEvent(BP, EventName);
    if (!TestNotNull(TEXT("the authored event node is in the graph"), Node))
    {
        DeleteAssetIfPresent(AssetPath);
        return true;
    }
    FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
    TestNull(TEXT("the event node is gone from the graph"), FindCustomEvent(BP, EventName));

    // 3. blueprint.get must not resurrect it from the registry.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), AssetPath);
        FTestResponseCapture Capture;
        if (TestTrue(TEXT("blueprint.get handler found"),
                InvokeHandlerWithCapture(TEXT("blueprint.get"), Payload, Capture)))
        {
            TestTrue(TEXT("blueprint.get succeeded"), Capture.bSuccess);
            TestFalse(TEXT("blueprint.get does not report an event the graph no longer has"),
                JsonArrayHasObjectWithStringField(
                    Capture.Result, TEXT("events"), TEXT("name"), EventName.ToString()));
        }
    }

    DeleteAssetIfPresent(AssetPath);
    return true;
}
