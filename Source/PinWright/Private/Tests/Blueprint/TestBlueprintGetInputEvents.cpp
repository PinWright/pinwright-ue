// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-blueprint-get-omits-inputkey-events. Both summary
// handlers use CollectBlueprintEvents, so one real InputKey node must surface
// through both routes with its key identity and its two connected exec edges.

#include "Misc/AutomationTest.h"
#include "Compiler/CodeNodeEmitter.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_InputTouch.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if __has_include("K2Node_InputVectorAxisEvent.h")
#include "K2Node_InputVectorAxisEvent.h"
#define PINWRIGHT_TEST_HAS_INPUT_VECTOR_AXIS_EVENT 1
#else
#define PINWRIGHT_TEST_HAS_INPUT_VECTOR_AXIS_EVENT 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGetInputEventsTest,
    "PinWright.blueprint.get.InputEventsReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGetInputEventsTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CompilerTestUtils::CreateTransientTestBP(TEXT("BlueprintGetInputEvents"));
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        return true;
    }

    UK2Node_InputKey* InputKeyNode = NewObject<UK2Node_InputKey>(EventGraph);
    InputKeyNode->InputKey = EKeys::LeftMouseButton;
    InputKeyNode->CreateNewGuid();
    InputKeyNode->PostPlacedNewNode();
    InputKeyNode->AllocateDefaultPins();
    EventGraph->AddNode(InputKeyNode, true, false);

    UK2Node_ExecutionSequence* PressedTarget = NewObject<UK2Node_ExecutionSequence>(EventGraph);
    PressedTarget->CreateNewGuid();
    PressedTarget->PostPlacedNewNode();
    PressedTarget->AllocateDefaultPins();
    EventGraph->AddNode(PressedTarget, true, false);

    UK2Node_ExecutionSequence* ReleasedTarget = NewObject<UK2Node_ExecutionSequence>(EventGraph);
    ReleasedTarget->CreateNewGuid();
    ReleasedTarget->PostPlacedNewNode();
    ReleasedTarget->AllocateDefaultPins();
    EventGraph->AddNode(ReleasedTarget, true, false);

    UEdGraphPin* PressedPin = InputKeyNode->GetPressedPin();
    UEdGraphPin* ReleasedPin = InputKeyNode->GetReleasedPin();
    UEdGraphPin* PressedTargetPin = PressedTarget->GetExecPin();
    UEdGraphPin* ReleasedTargetPin = ReleasedTarget->GetExecPin();
    if (TestNotNull(TEXT("InputKey Pressed pin exists"), PressedPin)
        && TestNotNull(TEXT("Pressed target exec pin exists"), PressedTargetPin))
    {
        PressedPin->MakeLinkTo(PressedTargetPin);
    }
    if (TestNotNull(TEXT("InputKey Released pin exists"), ReleasedPin)
        && TestNotNull(TEXT("Released target exec pin exists"), ReleasedTargetPin))
    {
        ReleasedPin->MakeLinkTo(ReleasedTargetPin);
    }

    auto AssertInputEvent = [this, Blueprint, PressedTarget, ReleasedTarget](
        const TCHAR* Method, const TCHAR* PathField)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(PathField, Blueprint->GetPathName());

        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s handler found"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestTrue(*FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject> InputEvent = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("events"), TEXT("name"), TEXT("LeftMouseButton"));
        if (!TestTrue(*FString::Printf(TEXT("%s events[] contains LeftMouseButton"), Method),
            InputEvent.IsValid()))
        {
            return;
        }

        FString EventType;
        TestTrue(TEXT("Input event carries eventType"),
            InputEvent->TryGetStringField(TEXT("eventType"), EventType));
        TestEqual(TEXT("Input event reports its authored node class"),
            EventType, FString(TEXT("K2Node_InputKey")));

        const TSharedPtr<FJsonObject> PressedOutput = JsonArrayFindObjectByStringField(
            InputEvent, TEXT("execOutputs"), TEXT("pin"), TEXT("Pressed"));
        const TSharedPtr<FJsonObject> ReleasedOutput = JsonArrayFindObjectByStringField(
            InputEvent, TEXT("execOutputs"), TEXT("pin"), TEXT("Released"));
        if (TestTrue(TEXT("Input event exposes its Pressed edge"), PressedOutput.IsValid()))
        {
            FString TargetNodeId;
            TestTrue(TEXT("Pressed edge carries targetNodeId"),
                PressedOutput->TryGetStringField(TEXT("targetNodeId"), TargetNodeId));
            TestEqual(TEXT("Pressed edge identifies its actual target"),
                TargetNodeId, PressedTarget->NodeGuid.ToString());
        }
        if (TestTrue(TEXT("Input event exposes its Released edge"), ReleasedOutput.IsValid()))
        {
            FString TargetNodeId;
            TestTrue(TEXT("Released edge carries targetNodeId"),
                ReleasedOutput->TryGetStringField(TEXT("targetNodeId"), TargetNodeId));
            TestEqual(TEXT("Released edge identifies its actual target"),
                TargetNodeId, ReleasedTarget->NodeGuid.ToString());
        }
    };

    AssertInputEvent(TEXT("blueprint.get"), TEXT("path"));
    AssertInputEvent(TEXT("blueprint.inspect"), TEXT("assetPath"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGetOtherInputEntryKindsTest,
    "PinWright.blueprint.get.OtherInputEntryKinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGetOtherInputEntryKindsTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CompilerTestUtils::CreateTransientTestBP(TEXT("BlueprintOtherInputEntryKinds"));
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        return true;
    }

    auto PrepareNode = [this, EventGraph](UK2Node* Node, const TCHAR* Label) -> bool
    {
        if (!TestNotNull(*FString::Printf(TEXT("%s node created"), Label), Node))
        {
            return false;
        }
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        EventGraph->AddNode(Node, true, false);
        return true;
    };

    auto MakeTarget = [this, EventGraph](const TCHAR* Label) -> UK2Node_ExecutionSequence*
    {
        UK2Node_ExecutionSequence* Target = NewObject<UK2Node_ExecutionSequence>(EventGraph);
        if (!TestNotNull(*FString::Printf(TEXT("%s target created"), Label), Target))
        {
            return nullptr;
        }
        Target->CreateNewGuid();
        Target->PostPlacedNewNode();
        Target->AllocateDefaultPins();
        EventGraph->AddNode(Target, true, false);
        return Target;
    };

    auto ConnectFirstExecOutput = [this](UK2Node* Source,
                                          UK2Node_ExecutionSequence* Target,
                                          const TCHAR* Label) -> bool
    {
        if (!Source || !Target)
        {
            return false;
        }
        UEdGraphPin* TargetPin = Target->GetExecPin();
        if (!TestNotNull(*FString::Printf(TEXT("%s target exec pin exists"), Label), TargetPin))
        {
            return false;
        }
        for (UEdGraphPin* Pin : Source->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                Pin->MakeLinkTo(TargetPin);
                return true;
            }
        }
        TestTrue(*FString::Printf(TEXT("%s exposes an exec output"), Label), false);
        return false;
    };

    struct FExpectedInputEntry
    {
        FString Name;
        FString EventType;
        FGuid TargetGuid;
        FString Label;
    };
    TArray<FExpectedInputEntry> ExpectedEntries;
    auto RecordEntry = [&ExpectedEntries](const FString& Name,
                                           const FString& EventType,
                                           const FGuid& TargetGuid,
                                           const TCHAR* Label)
    {
        FExpectedInputEntry Entry;
        Entry.Name = Name;
        Entry.EventType = EventType;
        Entry.TargetGuid = TargetGuid;
        Entry.Label = Label;
        ExpectedEntries.Add(MoveTemp(Entry));
    };

    UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(EventGraph);
    if (InputActionNode)
    {
        InputActionNode->InputActionName = FName(TEXT("PinWrightFireAction"));
        if (PrepareNode(InputActionNode, TEXT("InputAction")))
        {
            UK2Node_ExecutionSequence* Target = MakeTarget(TEXT("InputAction"));
            ConnectFirstExecOutput(InputActionNode, Target, TEXT("InputAction"));
            if (Target)
            {
                RecordEntry(InputActionNode->InputActionName.ToString(),
                    InputActionNode->GetClass()->GetName(), Target->NodeGuid, TEXT("InputAction"));
            }
        }
    }

    UK2Node_InputAxisEvent* InputAxisNode = NewObject<UK2Node_InputAxisEvent>(EventGraph);
    if (InputAxisNode)
    {
        InputAxisNode->InputAxisName = FName(TEXT("PinWrightMoveForward"));
        if (PrepareNode(InputAxisNode, TEXT("InputAxisEvent")))
        {
            UK2Node_ExecutionSequence* Target = MakeTarget(TEXT("InputAxisEvent"));
            ConnectFirstExecOutput(InputAxisNode, Target, TEXT("InputAxisEvent"));
            if (Target)
            {
                RecordEntry(InputAxisNode->InputAxisName.ToString(),
                    InputAxisNode->GetClass()->GetName(), Target->NodeGuid, TEXT("InputAxisEvent"));
            }
        }
    }

    UK2Node_InputAxisKeyEvent* InputAxisKeyNode = NewObject<UK2Node_InputAxisKeyEvent>(EventGraph);
    if (InputAxisKeyNode)
    {
        InputAxisKeyNode->AxisKey = EKeys::MouseX;
        if (PrepareNode(InputAxisKeyNode, TEXT("InputAxisKeyEvent")))
        {
            UK2Node_ExecutionSequence* Target = MakeTarget(TEXT("InputAxisKeyEvent"));
            ConnectFirstExecOutput(InputAxisKeyNode, Target, TEXT("InputAxisKeyEvent"));
            if (Target)
            {
                RecordEntry(InputAxisKeyNode->AxisKey.GetFName().ToString(),
                    InputAxisKeyNode->GetClass()->GetName(), Target->NodeGuid, TEXT("InputAxisKeyEvent"));
            }
        }
    }

#if PINWRIGHT_TEST_HAS_INPUT_VECTOR_AXIS_EVENT
    UK2Node_InputVectorAxisEvent* InputVectorAxisNode =
        NewObject<UK2Node_InputVectorAxisEvent>(EventGraph);
    if (InputVectorAxisNode)
    {
        InputVectorAxisNode->AxisKey = EKeys::Gamepad_Left2D;
        if (PrepareNode(InputVectorAxisNode, TEXT("InputVectorAxisEvent")))
        {
            UK2Node_ExecutionSequence* Target = MakeTarget(TEXT("InputVectorAxisEvent"));
            ConnectFirstExecOutput(InputVectorAxisNode, Target, TEXT("InputVectorAxisEvent"));
            if (Target)
            {
                RecordEntry(InputVectorAxisNode->AxisKey.GetFName().ToString(),
                    InputVectorAxisNode->GetClass()->GetName(), Target->NodeGuid,
                    TEXT("InputVectorAxisEvent"));
            }
        }
    }
#endif

    UK2Node_InputTouch* InputTouchNode = NewObject<UK2Node_InputTouch>(EventGraph);
    if (InputTouchNode && PrepareNode(InputTouchNode, TEXT("InputTouch")))
    {
        UK2Node_ExecutionSequence* Target = MakeTarget(TEXT("InputTouch"));
        ConnectFirstExecOutput(InputTouchNode, Target, TEXT("InputTouch"));
        if (Target)
        {
            RecordEntry(TEXT("Touch"), InputTouchNode->GetClass()->GetName(),
                Target->NodeGuid, TEXT("InputTouch"));
        }
    }

    UPackage* ActionPackage = nullptr;
    UInputAction* InputAction = nullptr;
    TStrongObjectPtr<UPackage> ActionPackageOwner;
    TStrongObjectPtr<UInputAction> InputActionOwner;
    FString InputActionObjectPath;
    ON_SCOPE_EXIT
    {
        if (!InputActionObjectPath.IsEmpty())
        {
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(InputActionObjectPath);
        }
        if (InputAction)
        {
            InputAction->ClearFlags(RF_Public | RF_Standalone);
            InputAction->SetFlags(RF_Transient);
            InputAction->MarkAsGarbage();
        }
        if (ActionPackage)
        {
            ActionPackage->ClearFlags(RF_Public | RF_Standalone);
            ActionPackage->SetFlags(RF_Transient);
            ActionPackage->MarkAsGarbage();
        }
        InputActionOwner.Reset();
        ActionPackageOwner.Reset();
        if (InputAction || ActionPackage)
        {
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        }
    };

    if (!FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("enhanced-input-node-unavailable"),
            TEXT("InputBlueprintNodes is unavailable; skipping only Enhanced Input entry coverage."));
    }
    else
    {
        const FString ActionPackagePath = FString::Printf(
            TEXT("/Temp/PinWrightTests/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        ActionPackage = CreatePackage(*ActionPackagePath);
        if (ActionPackage)
        {
            ActionPackage->SetFlags(RF_Transient);
        }
        const FName ActionName(*FPackageName::GetLongPackageAssetName(ActionPackagePath));
        InputAction = ActionPackage
            ? NewObject<UInputAction>(ActionPackage, ActionName, RF_Public | RF_Standalone)
            : nullptr;
        ActionPackageOwner.Reset(ActionPackage);
        InputActionOwner.Reset(InputAction);
        InputActionObjectPath = InputAction ? InputAction->GetPathName() : FString();
        if (InputAction)
        {
            InputAction->ValueType = EInputActionValueType::Axis2D;
            InputAction->ClearFlags(RF_Standalone);
            FAssetRegistryModule::AssetCreated(InputAction);
        }
        if (TestNotNull(TEXT("Enhanced Input fixture created"), InputAction))
        {
            FCodeNodeEmitter Emitter(Blueprint, EventGraph);
            UEdGraphNode* EnhancedNode = Emitter.CreateEnhancedInputActionNode(InputAction);
            if (TestNotNull(TEXT("Enhanced Input node created"), EnhancedNode))
            {
                UK2Node_ExecutionSequence* Target = MakeTarget(TEXT("EnhancedInputAction"));
                ConnectFirstExecOutput(Cast<UK2Node>(EnhancedNode), Target,
                    TEXT("EnhancedInputAction"));
                if (Target)
                {
                    RecordEntry(InputAction->GetPathName(), EnhancedNode->GetClass()->GetName(),
                        Target->NodeGuid, TEXT("EnhancedInputAction"));
                }
            }
        }
    }

    auto AssertEntries = [this, Blueprint, &ExpectedEntries](
        const TCHAR* Method, const TCHAR* PathField)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(PathField, Blueprint->GetPathName());

        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s handler found"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestTrue(*FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }

        for (const FExpectedInputEntry& Expected : ExpectedEntries)
        {
            const TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(
                Capture.Result, TEXT("events"), TEXT("name"), Expected.Name);
            if (!TestTrue(*FString::Printf(TEXT("%s events[] contains %s"), Method, *Expected.Label),
                Entry.IsValid()))
            {
                continue;
            }

            FString EventType;
            TestTrue(*FString::Printf(TEXT("%s %s carries eventType"), Method, *Expected.Label),
                Entry->TryGetStringField(TEXT("eventType"), EventType));
            TestEqual(*FString::Printf(TEXT("%s %s reports its authored node class"),
                Method, *Expected.Label), EventType, Expected.EventType);

            const TArray<TSharedPtr<FJsonValue>>* ExecOutputs = nullptr;
            if (!TestTrue(*FString::Printf(TEXT("%s %s carries execOutputs"), Method, *Expected.Label),
                Entry->TryGetArrayField(TEXT("execOutputs"), ExecOutputs) && ExecOutputs))
            {
                continue;
            }

            bool bTargetFound = false;
            const FString ExpectedTargetId = Expected.TargetGuid.ToString();
            for (const TSharedPtr<FJsonValue>& Value : *ExecOutputs)
            {
                const TSharedPtr<FJsonObject>* Output = nullptr;
                if (!Value.IsValid() || !Value->TryGetObject(Output)
                    || !Output || !(*Output).IsValid())
                {
                    continue;
                }
                FString TargetNodeId;
                if ((*Output)->TryGetStringField(TEXT("targetNodeId"), TargetNodeId)
                    && TargetNodeId == ExpectedTargetId)
                {
                    bTargetFound = true;
                    break;
                }
            }
            TestTrue(*FString::Printf(TEXT("%s %s identifies its connected target"),
                Method, *Expected.Label), bTargetFound);
        }
    };

    AssertEntries(TEXT("blueprint.get"), TEXT("path"));
    AssertEntries(TEXT("blueprint.inspect"), TEXT("assetPath"));
    return true;
}
