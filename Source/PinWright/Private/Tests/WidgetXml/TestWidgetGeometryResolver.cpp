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
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/WidgetXml/TestWidgetConstructProbeFixture.h"

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

// ============================================================================
// 7. Offscreen tier builds a DESIGN-TIME instance: no runtime lifecycle hooks
// ============================================================================
//
// B-geometry-offscreen-runs-native-construct: the offscreen tier used CreateWidget, a runtime
// instance with no game instance behind it, so TakeWidget ran the C++ parent's NativeConstruct
// and a project widget reaching UGameplayMessageSubsystem::Get there asserted and killed the
// editor. The fixture parent counts the hooks instead of asserting.
//
// Counterfactual: restore `CreateWidget<UUserWidget>(World, GenClass)` in ResolveViaOffscreen
// and NativeConstructCalls reads 1 (OnWidgetRebuilt calls it whenever !IsDesignTime()), failing
// the "NativeConstruct never ran" assertion; drop the SetDesignerFlags calls and it reads 1 too.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGeometryResolverOffscreenSkipsNativeConstructTest,
    "PinWright.widget_geometry.offscreen.DoesNotRunNativeConstruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGeometryResolverOffscreenSkipsNativeConstructTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("WBP_OffscreenConstructProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(*(FString(TEXT("/Game/PinWrightTests/")) + AssetName));
    if (!TestNotNull(TEXT("package created"), Package))
    {
        return false;
    }
    Package->SetFlags(RF_Transient);

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UTestWidgetConstructProbe::StaticClass(), Package, *AssetName, BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardLoadedAssetNoGc(WBP);
    };
    if (!WBP || !WBP->WidgetTree)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: OffscreenSkipsNativeConstruct; CreateBlueprint returned no widget blueprint, so the offscreen tier was NOT exercised."));
        return true;
    }
    UTextBlock* Label = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("ProbeLabel"));
    WBP->WidgetTree->RootWidget = Label;
    FKismetEditorUtilities::CompileBlueprint(WBP);
    if (!TestNotNull(TEXT("fixture compiled"), WBP->GeneratedClass.Get()))
    {
        return false;
    }

    // Never opened in a Designer and never added to a viewport, so the waterfall reaches the
    // offscreen tier - asserted below rather than assumed.
    UTestWidgetConstructProbe::ResetCounters();
    FWidgetGeometryRequest Request;
    Request.Blueprint = WBP;
    FWidgetGeometryResult Result = FWidgetGeometryResolver::Resolve(Request);

    TestEqual(TEXT("no top-level error"), Result.TopLevelError, FString());
    TestTrue(TEXT("served by the offscreen tier"),
        Result.ServedBy == EWidgetGeometrySource::Offscreen);
    TestEqual(TEXT("NativeConstruct never ran on the transient instance"),
        UTestWidgetConstructProbe::NativeConstructCalls, 0);
    TestEqual(TEXT("NativeOnInitialized never ran on the transient instance"),
        UTestWidgetConstructProbe::NativeOnInitializedCalls, 0);

    UUserWidget* Root = Result.GetLiveRoot();
    if (TestNotNull(TEXT("offscreen root kept alive on the result"), Root))
    {
        TestTrue(TEXT("offscreen root is a design-time instance"), Root->IsDesignTime());
        TMap<FName, UWidget*> NameIndex;
        FWidgetGeometryResolver::BuildNameIndex(Root, NameIndex);
        UWidget* const* LiveLabel = NameIndex.Find(TEXT("ProbeLabel"));
        const FResolvedGeometry* LabelGeo = LiveLabel
            ? Result.ByWidget.Find(FObjectKey(*LiveLabel)) : nullptr;
        if (TestNotNull(TEXT("child widget measured"), LabelGeo))
        {
            // Still a real measurement: design-time must not cost the layout pass.
            TestTrue(TEXT("child geometry ok"), LabelGeo->Status == FResolvedGeometry::EStatus::Ok);
        }
    }
    return true;
}
