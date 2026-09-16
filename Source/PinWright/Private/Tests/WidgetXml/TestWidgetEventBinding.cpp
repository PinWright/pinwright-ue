// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for widget_event BPIR binding on a Button that was added via
// widget.import_xml into a widget blueprint that already has a prior bound button.
//
// The fix: CodeNodeEmitter::CreateComponentEventNode prefers SkeletonGeneratedClass
// over GeneratedClass when resolving the widget-variable FObjectProperty.
// widget.import_xml calls MarkBlueprintAsStructurallyModified which regenerates the
// skeleton but leaves GeneratedClass stale until a full compile. If the emitter
// looked at GeneratedClass it would miss the new widget's property, producing an
// invalid BndEvt binding (DelegateOwnerClass pointing at the wrong class).
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "UObject/GarbageCollection.h"
#include "Misc/PackageName.h"
#include "K2Node_ComponentBoundEvent.h"
#include "EdGraph/EdGraph.h"


using WidgetXmlTestHelpers::MakeXmlTestAssetPath;
using WidgetXmlTestHelpers::CreateXmlTestWidget;
using WidgetXmlTestHelpers::CleanupXmlTestAsset;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEventBindingXmlImportedButtonTest,
    "PinWright.widget.bind_event.XmlImportedButtonSiblingBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetEventBindingXmlImportedButtonTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlImportedEvent"));
    FTestResponseCapture Capture;

    // 1. Create a widget blueprint.
    const bool bCreateFound = CreateXmlTestWidget(AssetPath, Capture);
    TestTrue(TEXT("create handler found"), bCreateFound);
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);
    if (!bCreateFound || !Capture.bSuccess)
    {
        CleanupXmlTestAsset(AssetPath);
        return false;
    }

    // 2. Add a CanvasPanel root.
    {
        TSharedPtr<FJsonObject> AddCanvas = MakeShared<FJsonObject>();
        AddCanvas->SetStringField(TEXT("widgetPath"), AssetPath);
        AddCanvas->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
        AddCanvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddCanvas, Capture);
        // Newly-created widget blueprints often come with an auto root CanvasPanel;
        // if that name collides this call may fail, which is fine as long as a root
        // canvas exists for step 3. Don't hard-fail the test here.
    }

    // 3. Add the first Button that will receive a binding BEFORE any XML import.
    {
        TSharedPtr<FJsonObject> AddFirst = MakeShared<FJsonObject>();
        AddFirst->SetStringField(TEXT("widgetPath"), AssetPath);
        AddFirst->SetStringField(TEXT("type"), TEXT("Button"));
        AddFirst->SetStringField(TEXT("name"), TEXT("FirstButton"));
        AddFirst->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddFirst, Capture);
        TestTrue(TEXT("add FirstButton succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    // 4. Compile a BPIR widget_event for FirstButton in replace mode so the BP has a
    //    pre-existing sibling BndEvt node prior to the import that triggers the bug.
    {
        TSharedPtr<FJsonObject> Compile1 = MakeShared<FJsonObject>();
        Compile1->SetStringField(TEXT("assetPath"), AssetPath);
        Compile1->SetStringField(TEXT("mode"), TEXT("replace"));
        Compile1->SetStringField(TEXT("code"),
            TEXT("entry widget_event FirstButton.OnClicked() {\n")
            TEXT("    call PrintString(InString: \"first\")\n")
            TEXT("}"));
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Compile1, Capture);
        TestTrue(TEXT("first compile succeeded"), Capture.bSuccess);
        bool bFirstCompiled = false;
        if (Capture.Result.IsValid()) Capture.Result->TryGetBoolField(TEXT("compiled"), bFirstCompiled);
        TestTrue(TEXT("first compile reports compiled: true"), bFirstCompiled);
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    // 5. Import a second Button via widget.import_xml (mode=add under the existing canvas).
    //    This path calls MarkBlueprintAsStructurallyModified, regenerating only the skeleton —
    //    the condition that triggers the stale-GeneratedClass bug in CreateComponentEventNode.
    {
        TSharedPtr<FJsonObject> Import = MakeShared<FJsonObject>();
        Import->SetStringField(TEXT("widgetPath"), AssetPath);
        Import->SetStringField(TEXT("mode"), TEXT("add"));
        Import->SetStringField(TEXT("targetName"), TEXT("RootCanvas"));
        Import->SetStringField(TEXT("xml"), TEXT("<Button Name=\"SecondButton\"/>"));
        InvokeHandlerWithCapture(TEXT("widget.import_xml"), Import, Capture);
        TestTrue(TEXT("import SecondButton succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* VarWidgets = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("variableWidgets"), VarWidgets))
            {
                bool bHasSecond = false;
                for (const TSharedPtr<FJsonValue>& V : *VarWidgets)
                {
                    if (V.IsValid() && V->AsString() == TEXT("SecondButton"))
                    {
                        bHasSecond = true;
                        break;
                    }
                }
                TestTrue(TEXT("variableWidgets contains SecondButton"), bHasSecond);
            }
        }
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    // 6. Compile a widget_event for SecondButton in append mode. This is the call that
    //    regressed: CreateComponentEventNode used to read GeneratedClass (stale post-import)
    //    and miss SecondButton's FObjectProperty, producing an invalid BndEvt binding.
    {
        TSharedPtr<FJsonObject> Compile2 = MakeShared<FJsonObject>();
        Compile2->SetStringField(TEXT("assetPath"), AssetPath);
        Compile2->SetStringField(TEXT("mode"), TEXT("append"));
        Compile2->SetStringField(TEXT("code"),
            TEXT("entry widget_event SecondButton.OnClicked() {\n")
            TEXT("    call PrintString(InString: \"second\")\n")
            TEXT("}"));
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Compile2, Capture);
        TestTrue(TEXT("second compile succeeded"), Capture.bSuccess);
        bool bSecondCompiled = false;
        if (Capture.Result.IsValid()) Capture.Result->TryGetBoolField(TEXT("compiled"), bSecondCompiled);
        TestTrue(TEXT("second compile reports compiled: true"), bSecondCompiled);
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    // 7. Verify the binding resolves to a valid delegate. widget.bind_event finds the
    //    BndEvt node and reports isDelegateValid based on the generated-class property
    //    + node's target delegate property — which is exactly what the fix keeps correct.
    {
        TSharedPtr<FJsonObject> Bind = MakeShared<FJsonObject>();
        Bind->SetStringField(TEXT("blueprintPath"), AssetPath);
        Bind->SetStringField(TEXT("widgetName"), TEXT("SecondButton"));
        Bind->SetStringField(TEXT("eventName"), TEXT("OnClicked"));
        InvokeHandlerWithCapture(TEXT("widget.bind_event"), Bind, Capture);
        TestTrue(TEXT("bind_event invocation succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bIsDelegateValid = false;
            Capture.Result->TryGetBoolField(TEXT("isDelegateValid"), bIsDelegateValid);
            TestTrue(TEXT("SecondButton.OnClicked binding is valid"), bIsDelegateValid);

            FString CustomFunctionName;
            Capture.Result->TryGetStringField(TEXT("customFunctionName"), CustomFunctionName);
            TestFalse(TEXT("customFunctionName is not empty"), CustomFunctionName.IsEmpty());
            TestTrue(TEXT("customFunctionName has BndEvt_ prefix"),
                CustomFunctionName.StartsWith(TEXT("BndEvt__")));
        }
    }

    CleanupXmlTestAsset(AssetPath);
    return true;
}

// -------------------------------------------------------------------------
// Regression test: CreateComponentEventNode must walk ancestor class chain
// when resolving FMulticastDelegateProperty.
//
// Scenario: the widget variable's class (e.g. W_ImageButton_C) inherits
// OnClicked from an ancestor (UCommonButtonBase via LyraButtonBase). The
// prior fix used FindFProperty without EFieldIteratorFlags::IncludeSuper,
// which only searched direct properties and returned null for inherited
// delegates. As a result, InitializeComponentBoundEventParams was skipped,
// DelegateOwnerClass was set to BPClass, and CustomFunctionName was "None".
//
// Fix: pass EFieldIteratorFlags::IncludeSuper to FindFProperty so the lookup
// traverses the full ancestor chain.
//
// This test approximates the inheritance scenario using UEditorUtilityButton
// (a concrete subclass of UButton available in the editor-only Blutility
// module) so that OnClicked lives on the parent class and the lookup must
// traverse the ancestor chain. UCommonButtonBase and ULyraButtonBase are both
// marked UCLASS(Abstract) and cannot be instantiated by widget.import_xml,
// which is why this test uses UEditorUtilityButton instead.
// -------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEventInheritedDelegateTest,
    "PinWright.widget.bind_event.InheritedDelegateBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetEventInheritedDelegateTest::RunTest(const FString& Parameters)
{
    // Create a fresh widget BP and XML-import a CommonButton widget whose
    // OnClicked delegate lives on an ancestor class (UCommonButtonBase).
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_InheritedDelegate"));
    FTestResponseCapture Capture;

    // 1. Create widget blueprint.
    const bool bCreateFound = CreateXmlTestWidget(AssetPath, Capture);
    TestTrue(TEXT("create handler found"), bCreateFound);
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);
    if (!bCreateFound || !Capture.bSuccess)
    {
        CleanupXmlTestAsset(AssetPath);
        return false;
    }

    // 2. Add root canvas.
    {
        TSharedPtr<FJsonObject> AddCanvas = MakeShared<FJsonObject>();
        AddCanvas->SetStringField(TEXT("widgetPath"), AssetPath);
        AddCanvas->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
        AddCanvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddCanvas, Capture);
        // Ignore result — auto-root may already exist.
    }

    // 3. XML-import an EditorUtilityButton widget. UButton declares OnClicked
    //    as a UPROPERTY multicast delegate; UEditorUtilityButton inherits it
    //    without declaring any delegates of its own. This exercises the
    //    ancestor-chain traversal fixed by EFieldIteratorFlags::IncludeSuper.
    {
        TSharedPtr<FJsonObject> Import = MakeShared<FJsonObject>();
        Import->SetStringField(TEXT("widgetPath"), AssetPath);
        Import->SetStringField(TEXT("mode"), TEXT("add"));
        Import->SetStringField(TEXT("targetName"), TEXT("RootCanvas"));
        Import->SetStringField(TEXT("xml"), TEXT("<EditorUtilityButton Name=\"TestButton\"/>"));
        InvokeHandlerWithCapture(TEXT("widget.import_xml"), Import, Capture);
        TestTrue(TEXT("import TestButton succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    // 4. Compile a widget_event for TestButton.OnClicked(). Before the fix,
    //    FindFProperty without IncludeSuper returned null for the inherited
    //    delegate property, causing CustomFunctionName to be "None".
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        Compile->SetStringField(TEXT("assetPath"), AssetPath);
        Compile->SetStringField(TEXT("mode"), TEXT("replace"));
        Compile->SetStringField(TEXT("code"),
            TEXT("entry widget_event TestButton.OnClicked() {\n")
            TEXT("    call PrintString(InString: \"ok\")\n")
            TEXT("}"));
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Compile, Capture);
        TestTrue(TEXT("compile_bpir succeeded"), Capture.bSuccess);
        bool bCompiled = false;
        if (Capture.Result.IsValid()) Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled);
        TestTrue(TEXT("compile reports compiled: true"), bCompiled);
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    // 5. Verify the BndEvt node has a valid delegate binding (the key assertion
    //    that fails without EFieldIteratorFlags::IncludeSuper).
    {
        TSharedPtr<FJsonObject> Bind = MakeShared<FJsonObject>();
        Bind->SetStringField(TEXT("blueprintPath"), AssetPath);
        Bind->SetStringField(TEXT("widgetName"), TEXT("TestButton"));
        Bind->SetStringField(TEXT("eventName"), TEXT("OnClicked"));
        InvokeHandlerWithCapture(TEXT("widget.bind_event"), Bind, Capture);
        TestTrue(TEXT("bind_event invocation succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bIsDelegateValid = false;
            Capture.Result->TryGetBoolField(TEXT("isDelegateValid"), bIsDelegateValid);
            TestTrue(TEXT("TestButton.OnClicked binding is valid (inherited delegate resolved)"),
                bIsDelegateValid);

            FString CustomFunctionName;
            Capture.Result->TryGetStringField(TEXT("customFunctionName"), CustomFunctionName);
            TestFalse(TEXT("customFunctionName is not empty"), CustomFunctionName.IsEmpty());
            TestTrue(TEXT("customFunctionName has BndEvt_ prefix"),
                CustomFunctionName.StartsWith(TEXT("BndEvt__")));
        }
    }

    CleanupXmlTestAsset(AssetPath);
    return true;
}

// -------------------------------------------------------------------------
// Regression test for B-widget-event-stale-class-ref-upsert-fail
//
// Re-emitting the same `entry widget_event Target.OnEvent()` via compile_bpir
// (regardless of mode) must upsert the existing K2Node_ComponentBoundEvent,
// not append a duplicate. The bug: BpirCompiler Phase 0 only ran in Replace
// mode and keyed ComponentEvent/WidgetEvent cleanup on UK2Node_CustomEvent
// casts — which never matched UK2Node_ComponentBoundEvent (it inherits from
// UK2Node_Event, not CustomEvent). Result: two bound events on the same
// widget.delegate pair, both firing on click, with the older one often
// holding references to a deleted class GUID and breaking BP compile.
//
// Fix: unconditional pass that scans UbergraphPages for matching
// K2Node_ComponentBoundEvent by (ComponentPropertyName, DelegatePropertyName)
// and deletes them + their exec subgraphs before the new entry is emitted.
// -------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEventUpsertNoDuplicateTest,
    "PinWright.widget.bind_event.UpsertNoDuplicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetEventUpsertNoDuplicateTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_EventUpsert"));
    FTestResponseCapture Capture;

    const bool bCreated = CreateXmlTestWidget(AssetPath, Capture);
    TestTrue(TEXT("create handler found"), bCreated);
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);
    if (!bCreated || !Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }

    // Add root canvas (ignore result; auto-root may preempt this).
    {
        TSharedPtr<FJsonObject> AddCanvas = MakeShared<FJsonObject>();
        AddCanvas->SetStringField(TEXT("widgetPath"), AssetPath);
        AddCanvas->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
        AddCanvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddCanvas, Capture);
    }

    // Add a Button under the canvas.
    {
        TSharedPtr<FJsonObject> AddBtn = MakeShared<FJsonObject>();
        AddBtn->SetStringField(TEXT("widgetPath"), AssetPath);
        AddBtn->SetStringField(TEXT("type"), TEXT("Button"));
        AddBtn->SetStringField(TEXT("name"), TEXT("MyButton"));
        AddBtn->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddBtn, Capture);
        TestTrue(TEXT("add MyButton succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess) { CleanupXmlTestAsset(AssetPath); return false; }
    }

    auto CompileEvent = [&](const FString& Mode)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("mode"), Mode);
        Payload->SetStringField(TEXT("code"),
            TEXT("entry widget_event MyButton.OnClicked() {\n")
            TEXT("    call PrintString(InString: \"click\")\n")
            TEXT("}"));
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture);
    };

    auto CountBoundEventsForMyButton = [&]() -> int32
    {
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(AssetPath + TEXT(".") +
            FPackageName::GetLongPackageAssetName(AssetPath));
        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
        if (!WBP) return -1;
        int32 Count = 0;
        for (UEdGraph* Graph : WBP->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_ComponentBoundEvent* BE = Cast<UK2Node_ComponentBoundEvent>(Node);
                if (BE && BE->ComponentPropertyName == FName(TEXT("MyButton"))
                    && BE->DelegatePropertyName == FName(TEXT("OnClicked")))
                {
                    ++Count;
                }
            }
        }
        return Count;
    };

    // First compile creates the bound event.
    CompileEvent(TEXT("append"));
    TestTrue(TEXT("first compile succeeded"), Capture.bSuccess);
    TestEqual(TEXT("one bound event after first compile"), CountBoundEventsForMyButton(), 1);

    // Second compile of the identical body in append mode must upsert, not duplicate.
    CompileEvent(TEXT("append"));
    TestTrue(TEXT("second compile succeeded"), Capture.bSuccess);
    TestEqual(TEXT("still one bound event after second compile (append upsert)"),
        CountBoundEventsForMyButton(), 1);

    // Third compile in replace mode: still one.
    CompileEvent(TEXT("replace"));
    TestTrue(TEXT("third compile succeeded"), Capture.bSuccess);
    TestEqual(TEXT("still one bound event after replace compile"),
        CountBoundEventsForMyButton(), 1);

    CleanupXmlTestAsset(AssetPath);
    return true;
}
