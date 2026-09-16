// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FWidgetGeometryResolver::ParseRequest and ContainsExtensionPointWidget.
//
// Counterfactual for ParseRequest tests:
//   If the default-off behavior is reverted, null input returns true and every
//   widget.describe call silently runs an offscreen rebuild on the target widget's
//   tree, bypassing the extension-point guard and restoring the crash risk.
//
// Counterfactual for the detection test:
//   If ContainsExtensionPointWidget is removed, ResolveViaOffscreen proceeds to
//   TakeWidget(), which invokes UUIExtensionPointWidget::RebuildWidget →
//   dereferences null UCommonLocalPlayer → editor crash.

#include "Misc/AutomationTest.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"

// UUIExtensionPointWidget lives in the UIExtension plugin, which is not part of a stock
// UE 5.4-5.7 install (it ships with Lyra-style sample projects). The production resolver
// detects it by class path string and never links the module; only the construction-based
// detection tests below need the concrete type. Probe for the header and compile those two
// tests in only when it is available.
#if __has_include("Widgets/UIExtensionPointWidget.h")
#include "Widgets/UIExtensionPointWidget.h"
#define MCP_UI_EXTENSION_AVAILABLE 1
#else
#define MCP_UI_EXTENSION_AVAILABLE 0
#endif

// ============================================================================
// 1. ParseRequest — null input (default off)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverParseRequestDefaultOffTest,
    "PinWright.widget_geometry.parse_request.DefaultOff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverParseRequestDefaultOffTest::RunTest(const FString& Parameters)
{
    FWidgetGeometryRequest Req;
    const bool bEnabled = FWidgetGeometryResolver::ParseRequest(nullptr, nullptr, Req);
    TestFalse(TEXT("null GeoValue disables geometry (default off)"), bEnabled);
    return true;
}

// ============================================================================
// 2. ParseRequest — bool true enables with defaults
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverParseRequestBoolTrueTest,
    "PinWright.widget_geometry.parse_request.BoolTrue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverParseRequestBoolTrueTest::RunTest(const FString& Parameters)
{
    FWidgetGeometryRequest Req;
    TSharedPtr<FJsonValue> TrueValue = MakeShared<FJsonValueBoolean>(true);
    const bool bEnabled = FWidgetGeometryResolver::ParseRequest(TrueValue, nullptr, Req);
    TestTrue(TEXT("bool true enables geometry"), bEnabled);
    return true;
}

// ============================================================================
// 3. ParseRequest — bool false disables
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverParseRequestBoolFalseTest,
    "PinWright.widget_geometry.parse_request.BoolFalse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverParseRequestBoolFalseTest::RunTest(const FString& Parameters)
{
    FWidgetGeometryRequest Req;
    TSharedPtr<FJsonValue> FalseValue = MakeShared<FJsonValueBoolean>(false);
    const bool bEnabled = FWidgetGeometryResolver::ParseRequest(FalseValue, nullptr, Req);
    TestFalse(TEXT("bool false disables geometry"), bEnabled);
    return true;
}

// ============================================================================
// 4. ParseRequest — object with viewport_size and force_visible_for_measure
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverParseRequestObjectTest,
    "PinWright.widget_geometry.parse_request.Object",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverParseRequestObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> VpSizeObj = MakeShared<FJsonObject>();
    VpSizeObj->SetNumberField(TEXT("w"), 800.0);
    VpSizeObj->SetNumberField(TEXT("h"), 600.0);

    TSharedPtr<FJsonObject> GeoObj = MakeShared<FJsonObject>();
    GeoObj->SetObjectField(TEXT("viewport_size"), VpSizeObj);
    GeoObj->SetBoolField(TEXT("force_visible_for_measure"), true);

    TSharedPtr<FJsonValue> GeoValue = MakeShared<FJsonValueObject>(GeoObj);

    FWidgetGeometryRequest Req;
    const bool bEnabled = FWidgetGeometryResolver::ParseRequest(GeoValue, nullptr, Req);

    TestTrue(TEXT("object form enables geometry"), bEnabled);
    TestTrue(TEXT("bForceVisibleForMeasure is true"), Req.bForceVisibleForMeasure);
    TestEqual(TEXT("ViewportSize.X is 800"), Req.ViewportSize.X, 800.0);
    TestEqual(TEXT("ViewportSize.Y is 600"), Req.ViewportSize.Y, 600.0);
    return true;
}

// ============================================================================
// 5. ContainsExtensionPointWidget detection
// ============================================================================

#if MCP_UI_EXTENSION_AVAILABLE

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverExtensionPointDetectionTest,
    "PinWright.widget_geometry.ExtensionPointDetection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverExtensionPointDetectionTest::RunTest(const FString& Parameters)
{
    // --- Tree WITH a UUIExtensionPointWidget ---
    {
        UWidgetTree* TreeWithExt = NewObject<UWidgetTree>((UObject*)GetTransientPackage(), NAME_None, RF_Transient);

        UCanvasPanel* Canvas = NewObject<UCanvasPanel>(TreeWithExt, NAME_None, RF_Transient);
        TreeWithExt->RootWidget = Canvas;
        UUIExtensionPointWidget* ExtPoint = NewObject<UUIExtensionPointWidget>(TreeWithExt, NAME_None, RF_Transient);
        Canvas->AddChild(ExtPoint);

        const bool bDetected = FWidgetGeometryResolver::ContainsExtensionPointWidget(TreeWithExt);
        TestTrue(TEXT("DetectsUIExtensionPointWidget in tree"), bDetected);
    }

    // --- Tree WITHOUT a UUIExtensionPointWidget ---
    {
        UWidgetTree* TreeWithout = NewObject<UWidgetTree>((UObject*)GetTransientPackage(), NAME_None, RF_Transient);

        UCanvasPanel* Canvas = NewObject<UCanvasPanel>(TreeWithout, NAME_None, RF_Transient);
        TreeWithout->RootWidget = Canvas;
        UTextBlock* Text = NewObject<UTextBlock>(TreeWithout, NAME_None, RF_Transient);
        Canvas->AddChild(Text);

        const bool bDetected = FWidgetGeometryResolver::ContainsExtensionPointWidget(TreeWithout);
        TestFalse(TEXT("NoFalsePositiveOnPlainTree"), bDetected);
    }

    return true;
}

// ============================================================================
// 6. ContainsExtensionPointWidget — nested UserWidget tree
// ============================================================================
//
// Counterfactual: if WidgetGeometryResolver.cpp:274 reverts to ForEachWidget, this
// test fails because UE's ForEachWidget does not cross UUserWidget tree boundaries
// (WidgetTree.h:69-74), so the nested UUIExtensionPointWidget is never visited and
// ContainsExtensionPointWidget returns false. ForEachWidgetAndDescendants is the
// correct utility — it descends into UserWidgetChild->WidgetTree.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverExtensionPointDetectionNestedTest,
    "PinWright.widget_geometry.ExtensionPointDetectionNested",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverExtensionPointDetectionNestedTest::RunTest(const FString& Parameters)
{
    // Outer tree: Canvas → UUserWidget child.
    UWidgetTree* OuterTree = NewObject<UWidgetTree>((UObject*)GetTransientPackage(), NAME_None, RF_Transient);
    UCanvasPanel* OuterCanvas = NewObject<UCanvasPanel>(OuterTree, NAME_None, RF_Transient);
    OuterTree->RootWidget = OuterCanvas;

    UUserWidget* NestedUW = NewObject<UUserWidget>(OuterTree, NAME_None, RF_Transient);
    OuterCanvas->AddChild(NestedUW);

    // Nested user widget owns its own WidgetTree (mirrors how Initialize() populates
    // child UUserWidget instances during CreateWidget).
    NestedUW->WidgetTree = NewObject<UWidgetTree>(NestedUW, NAME_None, RF_Transactional);
    UCanvasPanel* NestedCanvas = NewObject<UCanvasPanel>(NestedUW->WidgetTree, NAME_None, RF_Transient);
    NestedUW->WidgetTree->RootWidget = NestedCanvas;
    UUIExtensionPointWidget* ExtPoint = NewObject<UUIExtensionPointWidget>(NestedUW->WidgetTree, NAME_None, RF_Transient);
    NestedCanvas->AddChild(ExtPoint);

    const bool bDetected = FWidgetGeometryResolver::ContainsExtensionPointWidget(OuterTree);
    TestTrue(TEXT("Detects UIExtensionPointWidget nested in child UserWidget tree"), bDetected);
    return true;
}

#endif // MCP_UI_EXTENSION_AVAILABLE
