// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for widget.bind name resolution (board ticket
// B-widget-bind-event-suffix-never-binds).
//
// The verb used to decide property-vs-event with `bIsEvent = !FindPropertyByName(name)`. A delegate
// IS a property, so that lookup succeeded for every real bindable delegate and the event branch was
// reachable only for names ABSENT from the class - i.e. only for names guaranteed not to bind. The
// two observable consequences, both asserted below:
//
//   * UImage::OnMouseButtonDownEvent (the real property) was REJECTED with
//     "Cannot derive return type for property"; "OnMouseButtonDown" (which exists nowhere) was
//     ACCEPTED and persisted. Neither UWidgetBlueprintGeneratedClass::InitializeBindingsStatic nor
//     FDelegateEditorBinding::IsBindingValid ever appends "Event", so the accepted record resolved
//     to nothing and widget.export_xml rendered it as a genuine binding.
//   * UButton::OnClicked - the RPC's own documented example - is a MULTICAST delegate the Bindings
//     array cannot carry at all, and was likewise accepted.
//
// Both tests drive the dispatcher rather than the utils, because the contract lives on the response.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/WidgetXml/WidgetXmlTestHelpers.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

// CreateXmlTestWidget just invokes widget.create_widget_blueprint; a real (compiled, skeleton-
// bearing) blueprint is required here because the fix verifies through the engine's own
// FDelegateEditorBinding::IsBindingValid, which resolves the handler on SkeletonGeneratedClass.
using WidgetXmlTestHelpers::MakeXmlTestAssetPath;
using WidgetXmlTestHelpers::CreateXmlTestWidget;
using WidgetXmlTestHelpers::CleanupXmlTestAsset;

namespace WidgetBindResolutionTestHelpers
{
    UWidgetBlueprint* FindBindTestWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return FindObject<UWidgetBlueprint>(nullptr, *(PackagePath + TEXT(".") + AssetName));
    }

    // Creates a widget blueprint with a canvas root. Returns false if the blueprint could not be
    // made; the caller cleans up either way.
    bool MakeBindProbeBlueprint(FAutomationTestBase& Test, const FString& AssetPath,
        FTestResponseCapture& Capture)
    {
        const bool bCreateFound = CreateXmlTestWidget(AssetPath, Capture);
        Test.TestTrue(TEXT("widget.create_widget_blueprint handler found"), bCreateFound);
        Test.TestTrue(TEXT("widget blueprint created"), Capture.bSuccess);
        if (!bCreateFound || !Capture.bSuccess)
        {
            return false;
        }

        // A freshly created widget blueprint usually already carries an auto root canvas; a name
        // collision here is harmless as long as a root exists for the children below.
        TSharedPtr<FJsonObject> AddCanvas = MakeShared<FJsonObject>();
        AddCanvas->SetStringField(TEXT("widgetPath"), AssetPath);
        AddCanvas->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
        AddCanvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddCanvas, Capture);
        return true;
    }

    bool AddBindProbeWidget(FAutomationTestBase& Test, const FString& AssetPath,
        const TCHAR* Type, const TCHAR* Name, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
        Add->SetStringField(TEXT("widgetPath"), AssetPath);
        Add->SetStringField(TEXT("type"), Type);
        Add->SetStringField(TEXT("name"), Name);
        Add->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), Add, Capture);
        Test.TestTrue(FString::Printf(TEXT("widget.add %s succeeded"), Name), Capture.bSuccess);
        return Capture.bSuccess;
    }

    void InvokeWidgetBind(const FString& AssetPath, const TCHAR* WidgetName, const TCHAR* PropertyName,
        const TCHAR* FunctionName, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Bind = MakeShared<FJsonObject>();
        Bind->SetStringField(TEXT("widgetPath"), AssetPath);
        Bind->SetStringField(TEXT("widgetName"), WidgetName);
        Bind->SetStringField(TEXT("propertyName"), PropertyName);
        Bind->SetStringField(TEXT("functionName"), FunctionName);
        InvokeHandlerWithCapture(TEXT("widget.bind"), Bind, Capture);
    }

    FString ReadBindString(const FTestResponseCapture& Capture, const TCHAR* Field)
    {
        FString Value;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(Field, Value);
        }
        return Value;
    }

    bool ReadBindBool(const FTestResponseCapture& Capture, const TCHAR* Field)
    {
        bool bValue = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(Field, bValue);
        }
        return bValue;
    }

    // Returns the stored binding for (WidgetName, PropertyName), or null.
    const FDelegateEditorBinding* FindStoredWidgetBinding(const UWidgetBlueprint* WidgetBP,
        const TCHAR* WidgetName, const TCHAR* PropertyName)
    {
        if (!WidgetBP)
        {
            return nullptr;
        }
        for (const FDelegateEditorBinding& Binding : WidgetBP->Bindings)
        {
            if (Binding.ObjectName == WidgetName && Binding.PropertyName == FName(PropertyName))
            {
                return &Binding;
            }
        }
        return nullptr;
    }
}

using namespace WidgetBindResolutionTestHelpers;

// ---------------------------------------------------------------------------
// A property-binding stem and a bindable event delegate both bind AND resolve.
//
// Counterfactual: before the fix "OnMouseButtonDownEvent" came back BINDING_FAILED
// ("Cannot derive return type for property"), and "Text" was accepted but produced an impure
// handler that FDelegateEditorBinding::IsBindingValid rejects with "needs to be bound to a pure
// function" - so `resolves` could not have been true for either.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetBindBindableNamesResolveTest,
    "PinWright.widget.bind.BindableNamesResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetBindBindableNamesResolveTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_BindResolve"));
    FTestResponseCapture Capture;

    if (!MakeBindProbeBlueprint(*this, AssetPath, Capture))
    {
        CleanupXmlTestAsset(AssetPath);
        return false;
    }

    const bool bProbesAdded =
        AddBindProbeWidget(*this, AssetPath, TEXT("TextBlock"), TEXT("ProbeText"), Capture)
        && AddBindProbeWidget(*this, AssetPath, TEXT("Image"), TEXT("ProbeImage"), Capture);
    if (!bProbesAdded)
    {
        CleanupXmlTestAsset(AssetPath);
        return false;
    }

    // 1. Property binding: UTextBlock has no "Text" delegate, it has "TextDelegate", which is the
    //    spelling UMG appends. The handler function must be pure and return FText.
    {
        InvokeWidgetBind(AssetPath, TEXT("ProbeText"), TEXT("Text"), TEXT("GetProbeText"), Capture);
        TestTrue(TEXT("widget.bind Text succeeded"), Capture.bSuccess);
        TestEqual(TEXT("Text is a property binding"),
            ReadBindString(Capture, TEXT("bindingType")), FString(TEXT("property")));
        TestEqual(TEXT("Text resolves through TextDelegate"),
            ReadBindString(Capture, TEXT("delegateProperty")), FString(TEXT("TextDelegate")));
        TestEqual(TEXT("Text is stored under the stem, not the delegate name"),
            ReadBindString(Capture, TEXT("propertyName")), FString(TEXT("Text")));
        TestTrue(TEXT("Text binding passes FDelegateEditorBinding::IsBindingValid"),
            ReadBindBool(Capture, TEXT("resolves")));
        TestTrue(TEXT("Text binding landed in the Bindings array"),
            ReadBindBool(Capture, TEXT("bindingStored")));
    }

    // 2. Bindable event: UImage::OnMouseButtonDownEvent is the real property name, resolved
    //    verbatim. Its handler takes the delegate's parameters and returns FEventReply.
    {
        InvokeWidgetBind(AssetPath, TEXT("ProbeImage"), TEXT("OnMouseButtonDownEvent"),
            TEXT("OnProbeImageMouseDown"), Capture);
        TestTrue(TEXT("widget.bind OnMouseButtonDownEvent succeeded"), Capture.bSuccess);
        TestEqual(TEXT("OnMouseButtonDownEvent is an event binding"),
            ReadBindString(Capture, TEXT("bindingType")), FString(TEXT("event")));
        TestEqual(TEXT("OnMouseButtonDownEvent resolves verbatim"),
            ReadBindString(Capture, TEXT("delegateProperty")), FString(TEXT("OnMouseButtonDownEvent")));
        TestTrue(TEXT("event binding passes FDelegateEditorBinding::IsBindingValid"),
            ReadBindBool(Capture, TEXT("resolves")));
    }

    // 3. Both records are on the asset, tagged as function bindings. EBindingKind defaults to
    //    Property, which mislabelled every binding this verb wrote before the fix.
    {
        const UWidgetBlueprint* WidgetBP = FindBindTestWidgetBlueprint(AssetPath);
        TestNotNull(TEXT("widget blueprint resolves after binding"), WidgetBP);
        if (WidgetBP)
        {
            const FDelegateEditorBinding* TextBinding =
                FindStoredWidgetBinding(WidgetBP, TEXT("ProbeText"), TEXT("Text"));
            TestNotNull(TEXT("ProbeText.Text binding stored"), TextBinding);
            if (TextBinding)
            {
                TestEqual(TEXT("ProbeText.Text names its handler"),
                    TextBinding->FunctionName, FName(TEXT("GetProbeText")));
                TestTrue(TEXT("ProbeText.Text is a function binding"),
                    TextBinding->Kind == EBindingKind::Function);
            }

            const FDelegateEditorBinding* EventBinding =
                FindStoredWidgetBinding(WidgetBP, TEXT("ProbeImage"), TEXT("OnMouseButtonDownEvent"));
            TestNotNull(TEXT("ProbeImage.OnMouseButtonDownEvent binding stored"), EventBinding);
            if (EventBinding)
            {
                TestEqual(TEXT("ProbeImage.OnMouseButtonDownEvent names its handler"),
                    EventBinding->FunctionName, FName(TEXT("OnProbeImageMouseDown")));
            }
        }
    }

    CleanupXmlTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------
// A name UMG cannot resolve is rejected, and the rejection names what would have.
//
// Counterfactual: before the fix both calls below returned
// {success: true, bindingType: "event", created: true} and persisted a record that binds to
// nothing - the exact false success this ticket was filed for.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetBindUnresolvableNamesRejectedTest,
    "PinWright.widget.bind.UnresolvableNamesRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetBindUnresolvableNamesRejectedTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_BindReject"));
    FTestResponseCapture Capture;

    if (!MakeBindProbeBlueprint(*this, AssetPath, Capture))
    {
        CleanupXmlTestAsset(AssetPath);
        return false;
    }

    const bool bProbesAdded =
        AddBindProbeWidget(*this, AssetPath, TEXT("Image"), TEXT("ProbeImage"), Capture)
        && AddBindProbeWidget(*this, AssetPath, TEXT("Button"), TEXT("ProbeButton"), Capture);
    if (!bProbesAdded)
    {
        CleanupXmlTestAsset(AssetPath);
        return false;
    }

    // 1. The suffix-stripped spelling exists nowhere on the class. UMG would look up
    //    "OnMouseButtonDownDelegate" then "OnMouseButtonDown" and find neither.
    {
        InvokeWidgetBind(AssetPath, TEXT("ProbeImage"), TEXT("OnMouseButtonDown"),
            TEXT("OnProbeImageMouseDown"), Capture);
        TestTrue(TEXT("rejection was sent"), Capture.bWasCalled);
        TestFalse(TEXT("suffix-stripped name is not a success"), Capture.bSuccess);
        TestEqual(TEXT("suffix-stripped name error code"),
            Capture.ErrorCode, FString(TEXT("WIDGET_BINDING_NAME_UNRESOLVED")));
        TestTrue(TEXT("rejection names the delegate that would resolve"),
            Capture.Message.Contains(TEXT("OnMouseButtonDownEvent")));

        const TArray<TSharedPtr<FJsonValue>>* BindableEvents = nullptr;
        const bool bHasEventList = Capture.Result.IsValid()
            && Capture.Result->TryGetArrayField(TEXT("bindableEvents"), BindableEvents);
        TestTrue(TEXT("rejection publishes the bindable event set"), bHasEventList);
        if (bHasEventList)
        {
            bool bListsRealDelegate = false;
            for (const TSharedPtr<FJsonValue>& Entry : *BindableEvents)
            {
                if (Entry.IsValid() && Entry->AsString() == TEXT("OnMouseButtonDownEvent"))
                {
                    bListsRealDelegate = true;
                    break;
                }
            }
            TestTrue(TEXT("bindableEvents contains OnMouseButtonDownEvent"), bListsRealDelegate);
        }
    }

    // 2. A multicast delegate is a real event, but not one the Bindings array can carry. It gets a
    //    distinct code so a caller can route to compile_bpir without parsing the message.
    {
        InvokeWidgetBind(AssetPath, TEXT("ProbeButton"), TEXT("OnClicked"),
            TEXT("OnProbeButtonClicked"), Capture);
        TestFalse(TEXT("multicast event is not a success"), Capture.bSuccess);
        TestEqual(TEXT("multicast event error code"),
            Capture.ErrorCode, FString(TEXT("WIDGET_BINDING_IS_MULTICAST_EVENT")));
        TestTrue(TEXT("multicast rejection routes to compile_bpir"),
            Capture.Message.Contains(TEXT("compile_bpir")));
    }

    // 3. Nothing was written for either refusal.
    {
        const UWidgetBlueprint* WidgetBP = FindBindTestWidgetBlueprint(AssetPath);
        TestNotNull(TEXT("widget blueprint resolves after rejection"), WidgetBP);
        if (WidgetBP)
        {
            TestEqual(TEXT("no binding persisted for a name UMG cannot resolve"),
                WidgetBP->Bindings.Num(), 0);
        }
    }

    CleanupXmlTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------
// A binding on a widget with bIsVariable=false is accepted, and it resolves.
//
// Board ticket B-widget-bind-accepts-non-variable-widget proposed refusing this case, on the
// reading that UWidgetBlueprintGeneratedClass::InitializeBindingsStatic resolves ObjectName
// through the generated class's object-property map and a non-variable widget is absent from it.
// The first half is true; the second is not. FWidgetBlueprintCompilerContext generates a hidden
// variable for any widget a binding names:
//
//     bShouldGenerateVariable = Widget->bIsVariable
//         || Widget->IsA<UNamedSlot>()
//         || WidgetBP->Bindings.ContainsByPredicate(Binding.ObjectName == Widget->GetName());
//
// unchanged across 5.3-5.8 ("In the event there are bindings for a widget, but it's not marked as
// a variable, make it one, but hide it from the UI"). Writing the record is what creates the
// property the runtime lookup needs.
//
// This test exists to keep that refusal from being added: it drives the whole path the ticket
// claimed was dead - bind, compile, instantiate, UUserWidget::Initialize - and asserts the child
// widget's delegate is bound afterwards. Adding a bIsVariable gate to widget.bind fails it at the
// first assertion; the compiler clause disappearing upstream fails it at the last.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetBindNonVariableWidgetResolvesTest,
    "PinWright.widget.bind.NonVariableWidgetResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetBindNonVariableWidgetResolvesTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_BindNonVariable"));
    FTestResponseCapture Capture;

    ON_SCOPE_EXIT
    {
        CleanupXmlTestAsset(AssetPath);
    };

    if (!MakeBindProbeBlueprint(*this, AssetPath, Capture)
        || !AddBindProbeWidget(*this, AssetPath, TEXT("TextBlock"), TEXT("ProbeText"), Capture))
    {
        return false;
    }

    UWidgetBlueprint* WidgetBP = FindBindTestWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("widget blueprint resolves"), WidgetBP);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        return false;
    }

    // widget.add promotes what it creates; clearing the flag reproduces the state
    // widget.import_xml leaves behind for a node carrying IsVariable="false".
    UWidget* Probe = WidgetBP->WidgetTree->FindWidget(FName(TEXT("ProbeText")));
    TestNotNull(TEXT("probe widget is in the tree"), Probe);
    if (!Probe)
    {
        return false;
    }
    Probe->bIsVariable = false;

    InvokeWidgetBind(AssetPath, TEXT("ProbeText"), TEXT("Text"), TEXT("GetProbeText"), Capture);
    TestTrue(TEXT("widget.bind accepts a non-variable widget"), Capture.bSuccess);
    TestTrue(TEXT("binding passes FDelegateEditorBinding::IsBindingValid"),
        ReadBindBool(Capture, TEXT("resolves")));
    TestTrue(TEXT("binding landed in the Bindings array"),
        ReadBindBool(Capture, TEXT("bindingStored")));
    TestEqual(TEXT("exactly one binding was written"), WidgetBP->Bindings.Num(), 1);
    // The verb must not paper over the case by flipping the flag either - the point is that the
    // compiler, not widget.bind, is what makes a bound non-variable widget addressable.
    TestFalse(TEXT("widget.bind left bIsVariable alone"), static_cast<bool>(Probe->bIsVariable));
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("widget.bind failed: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(WidgetBP);

    UWidgetBlueprintGeneratedClass* GeneratedClass =
        Cast<UWidgetBlueprintGeneratedClass>(WidgetBP->GeneratedClass);
    TestNotNull(TEXT("compile produced a widget generated class"), GeneratedClass);
    if (!GeneratedClass)
    {
        return false;
    }

    // The map InitializeBindingsStatic consults is built by iterating FObjectPropertyBase on the
    // generated class, so this property existing IS the ObjectName lookup succeeding.
    FObjectPropertyBase* HiddenWidgetProperty =
        CastField<FObjectPropertyBase>(GeneratedClass->FindPropertyByName(FName(TEXT("ProbeText"))));
    TestNotNull(TEXT("compiler generated a hidden variable for the bound non-variable widget"),
        HiddenWidgetProperty);
    // A hidden variable is not blueprint-visible: that is the whole difference from bIsVariable.
    if (HiddenWidgetProperty)
    {
        TestFalse(TEXT("the generated variable stayed hidden from blueprint graphs"),
            HiddenWidgetProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));
    }

    UUserWidget* Instance = NewObject<UUserWidget>(GetTransientPackage(), GeneratedClass);
    TestNotNull(TEXT("user widget instance constructed"), Instance);
    if (!Instance)
    {
        return false;
    }
    Instance->AddToRoot();
    ON_SCOPE_EXIT
    {
        Instance->RemoveFromRoot();
    };

    // UUserWidget::Initialize routes into InitializeWidgetStatic -> InitializeBindingsStatic, the
    // engine's own binder. This is the call the ticket predicted would resolve to nothing.
    Instance->Initialize();

    UWidget* BoundWidget = Instance->WidgetTree != nullptr
        ? Instance->WidgetTree->FindWidget(FName(TEXT("ProbeText")))
        : nullptr;
    TestNotNull(TEXT("instance carries the probe widget"), BoundWidget);
    if (!BoundWidget)
    {
        return false;
    }

    FDelegateProperty* TextDelegate =
        FindFProperty<FDelegateProperty>(BoundWidget->GetClass(), TEXT("TextDelegate"));
    TestNotNull(TEXT("UTextBlock still exposes TextDelegate"), TextDelegate);
    if (!TextDelegate)
    {
        return false;
    }

    const FScriptDelegate* BoundDelegate = TextDelegate->GetPropertyValuePtr_InContainer(BoundWidget);
    TestNotNull(TEXT("delegate storage reachable on the instance"), BoundDelegate);
    if (!BoundDelegate)
    {
        return false;
    }
    TestTrue(TEXT("TextDelegate is bound after Initialize"), BoundDelegate->IsBound());
    TestEqual(TEXT("TextDelegate points at the handler widget.bind wrote"),
        BoundDelegate->GetFunctionName(), FName(TEXT("GetProbeText")));

    return true;
}
