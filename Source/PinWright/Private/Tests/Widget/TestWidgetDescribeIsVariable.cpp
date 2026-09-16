// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for board ticket B-widget-variable-guid-map-is-not-a-variable-set.
//
// widget.describe used to read UWidgetBlueprint::WidgetVariableNameToGuidMap on UE 5.6+ as if
// membership in it meant "this widget is a blueprint variable". It does not.
// FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids populates that map from
// WidgetBP->ForEachSourceWidget(...) with no bIsVariable test at all, so after ANY compile the map
// holds every widget in the tree and describe published isVariable: true for all of them -
// including nodes authored IsVariable="false". Silent wrong data on a normal path: a caller
// deciding whether a $Name reference resolves in BPIR got a yes for a widget the compiler never
// exposes.
//
// The test drives the dispatcher because the contract lives on the response, and it compiles the
// blueprint first because the compile is what fills the map. Before the fix the non-variable
// assertion below reads true.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/WidgetXml/WidgetXmlTestHelpers.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

using WidgetXmlTestHelpers::MakeXmlTestAssetPath;
using WidgetXmlTestHelpers::CreateXmlTestWidget;
using WidgetXmlTestHelpers::CleanupXmlTestAsset;

namespace WidgetDescribeIsVariableTestHelpers
{
    UWidgetBlueprint* FindDescribeTestWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return FindObject<UWidgetBlueprint>(nullptr, *(PackagePath + TEXT(".") + AssetName));
    }

    bool AddDescribeProbeWidget(FAutomationTestBase& Test, const FString& AssetPath,
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

    // Runs widget.describe rooted at WidgetName (so the response's "tree" IS that node) and reads
    // back its isVariable field. bOutFound reports whether the response carried one at all, so a
    // missing field can never be mistaken for a false.
    bool DescribeIsVariable(const FString& AssetPath, const TCHAR* WidgetName,
        FTestResponseCapture& Capture, bool& bOutFound)
    {
        bOutFound = false;

        TSharedPtr<FJsonObject> Describe = MakeShared<FJsonObject>();
        Describe->SetStringField(TEXT("widgetPath"), AssetPath);
        Describe->SetStringField(TEXT("widgetName"), WidgetName);
        InvokeHandlerWithCapture(TEXT("widget.describe"), Describe, Capture);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* Tree = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("tree"), Tree) || !Tree || !Tree->IsValid())
        {
            return false;
        }

        bool bIsVariable = false;
        bOutFound = (*Tree)->TryGetBoolField(TEXT("isVariable"), bIsVariable);
        return bIsVariable;
    }
}

using namespace WidgetDescribeIsVariableTestHelpers;

// ---------------------------------------------------------------------------
// After a compile, describe still separates variable from non-variable widgets.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDescribeNonVariableWidgetReportsFalseTest,
    "PinWright.widget.describe.NonVariableWidgetReportsFalseAfterCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDescribeNonVariableWidgetReportsFalseTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_DescribeIsVariable"));
    FTestResponseCapture Capture;

    ON_SCOPE_EXIT
    {
        CleanupXmlTestAsset(AssetPath);
    };

    const bool bCreateFound = CreateXmlTestWidget(AssetPath, Capture);
    TestTrue(TEXT("widget.create_widget_blueprint handler found"), bCreateFound);
    TestTrue(TEXT("widget blueprint created"), Capture.bSuccess);
    if (!bCreateFound || !Capture.bSuccess)
    {
        return false;
    }

    // A freshly created widget blueprint usually already carries an auto root canvas; a name
    // collision here is harmless as long as a RootCanvas exists for the probes below.
    TSharedPtr<FJsonObject> AddCanvas = MakeShared<FJsonObject>();
    AddCanvas->SetStringField(TEXT("widgetPath"), AssetPath);
    AddCanvas->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
    AddCanvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
    InvokeHandlerWithCapture(TEXT("widget.add"), AddCanvas, Capture);

    if (!AddDescribeProbeWidget(*this, AssetPath, TEXT("TextBlock"), TEXT("VariableText"), Capture)
        || !AddDescribeProbeWidget(*this, AssetPath, TEXT("TextBlock"), TEXT("PlainText"), Capture))
    {
        return false;
    }

    UWidgetBlueprint* WidgetBP = FindDescribeTestWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("widget blueprint resolves"), WidgetBP);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        return false;
    }

    // widget.add promotes what it creates; clearing the flag reproduces the state
    // widget.import_xml leaves behind for a node carrying IsVariable="false".
    UWidget* Plain = WidgetBP->WidgetTree->FindWidget(FName(TEXT("PlainText")));
    TestNotNull(TEXT("non-variable probe is in the tree"), Plain);
    if (!Plain)
    {
        return false;
    }
    Plain->bIsVariable = false;

    // The compile is the trigger: ValidateAndFixUpVariableGuids fills the guid map from every
    // source widget, which is what the old 5.6+ branch mistook for the variable set.
    FKismetEditorUtilities::CompileBlueprint(WidgetBP);

    UWidget* PlainAfterCompile = WidgetBP->WidgetTree != nullptr
        ? WidgetBP->WidgetTree->FindWidget(FName(TEXT("PlainText")))
        : nullptr;
    TestNotNull(TEXT("non-variable probe survives the compile"), PlainAfterCompile);
    if (!PlainAfterCompile)
    {
        return false;
    }

    // Preconditions, not the contract under test: the compiler leaves bIsVariable alone, and yet
    // the guid map holds the widget anyway. Those two lines together are the whole defect.
    TestFalse(TEXT("compile left bIsVariable false"),
        static_cast<bool>(PlainAfterCompile->bIsVariable));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    TestTrue(TEXT("compile put the non-variable widget in WidgetVariableNameToGuidMap"),
        WidgetBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("PlainText"))));
#endif

    bool bFound = false;

    const bool bPlainIsVariable = DescribeIsVariable(AssetPath, TEXT("PlainText"), Capture, bFound);
    TestTrue(TEXT("widget.describe published isVariable for the non-variable widget"), bFound);
    TestFalse(TEXT("widget.describe reports isVariable false for a non-variable widget"),
        bPlainIsVariable);

    const bool bVariableIsVariable =
        DescribeIsVariable(AssetPath, TEXT("VariableText"), Capture, bFound);
    TestTrue(TEXT("widget.describe published isVariable for the variable widget"), bFound);
    TestTrue(TEXT("widget.describe still reports isVariable true for a variable widget"),
        bVariableIsVariable);

    return true;
}
