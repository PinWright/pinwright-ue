// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/AssetDumpBuilder.h"
#include "Handlers/UI/WidgetXmlExporter.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/AssetDumpHandlerInternal.h"
#include "Utils/AssetDumpWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"


#include "UObject/Package.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "UObject/Interface.h"

// ============================================================================
// AssetDumpBuilder.MetaJsonShape
// BuildMetaJson on a USceneComponent CDO — checks all required keys are present
// and that the v8 shape omits the version stamps (pluginVersion, dumpSchemaVersion
// moved to .dumpcache.json so meta.json stays byte-stable across plugin releases).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderMetaJsonShapeTest,
    "PinWright.utils.asset_dump_builder.MetaJsonShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderMetaJsonShapeTest::RunTest(const FString& Parameters)
{
    UObject* Obj = USceneComponent::StaticClass()->GetDefaultObject();
    TestNotNull(TEXT("CDO is valid"), Obj);
    if (!Obj) return false;

    TSharedPtr<FJsonObject> Meta = AssetDumpBuilder::BuildMetaJson(Obj);
    TestNotNull(TEXT("Meta JSON is not null"), Meta.Get());
    if (!Meta.IsValid()) return false;

    // All required keys present
    TestTrue(TEXT("assetPath key"),     Meta->HasField(TEXT("assetPath")));
    TestTrue(TEXT("className key"),     Meta->HasField(TEXT("className")));
    TestTrue(TEXT("parentClass key"),   Meta->HasField(TEXT("parentClass")));
    TestFalse(TEXT("packageFlags key absent (v5 dropped degenerate field)"),
        Meta->HasField(TEXT("packageFlags")));
    TestFalse(TEXT("pluginVersion key absent (v8 moved versioning to .dumpcache.json)"),
        Meta->HasField(TEXT("pluginVersion")));

    // Counterfactual: if BuildMetaJson is reverted to re-emit `packageFlags`, `assetType`, or the v8-removed version stamps (`pluginVersion`, `dumpSchemaVersion`), the absence assertions flip to fail.
    TestFalse(TEXT("assetType key absent (v4 removed redundant field)"),
        Meta->HasField(TEXT("assetType")));

    // className resolves to the concrete UE class name on a CDO — not the literal "UObject".
    TestEqual(TEXT("className == SceneComponent"),
        Meta->GetStringField(TEXT("className")), FString(TEXT("SceneComponent")));

    // v6: kind derives from IsChildOf walk on Asset->GetClass(). USceneComponent is a child
    // of UActorComponent, so kind resolves to "Component".
    TestTrue(TEXT("kind key present (v6)"), Meta->HasField(TEXT("kind")));
    TestEqual(TEXT("kind == Component"),
        Meta->GetStringField(TEXT("kind")), FString(TEXT("Component")));

    const TSharedPtr<FJsonValue> BlueprintTypeValue = Meta->TryGetField(TEXT("blueprintType"));
    TestTrue(TEXT("blueprintType present on non-Blueprint asset (v7)"),
        BlueprintTypeValue.IsValid());
    TestTrue(TEXT("blueprintType is null on non-Blueprint asset (v7)"),
        BlueprintTypeValue.IsValid() && BlueprintTypeValue->Type == EJson::Null);

    // v6: BuildMetaJson alone does not attach propertiesStatus — that's added in
    // BuildAllFilesForAsset for non-BP assets. The bare meta call should leave it absent.
    TestFalse(TEXT("propertiesStatus absent on bare BuildMetaJson call (v6)"),
        Meta->HasField(TEXT("propertiesStatus")));

    TestFalse(TEXT("dumpSchemaVersion key absent (v8 moved versioning to .dumpcache.json)"),
        Meta->HasField(TEXT("dumpSchemaVersion")));

    return true;
}

// ============================================================================
// AssetDumpBuilder.BpirConcatOrder
// BuildBpirText emission ORDER, which is what the test name claims:
//   * ubergraph pages are emitted before function/macro graphs
//     (AssetDumpBuilder.cpp:224-227 runs before :242-243), and
//   * function and macro graphs are alpha-sorted by SortAndAppend's
//     Sorted.Sort(...) at AssetDumpBuilder.cpp:232.
// A synthetic transient Blueprint holds the graphs in DELIBERATELY non-alphabetical
// registration order, so deleting the sort reorders the output and the index
// comparisons below fail. StartsWith/Contains checks cannot see order at all, which
// is why they are kept only as a supplement on the real engine asset.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderBpirConcatOrderTest,
    "PinWright.utils.asset_dump_builder.BpirConcatOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderBpirConcatOrderTest::RunTest(const FString& Parameters)
{
    // Null guard: BuildBpirText on nullptr must return empty string without crash.
    FString NullResult = AssetDumpBuilder::BuildBpirText(nullptr);
    TestTrue(TEXT("BuildBpirText(nullptr) returns empty"), NullResult.IsEmpty());

    // ------------------------------------------------------------------
    // Ordering fixture: a synthetic Blueprint whose graphs are registered out of
    // alphabetical order. Empty graphs are enough — BuildBpirText emits the
    // "# ==== Graph: <name> (<kind>) ====" header for every non-null graph before the
    // decompiler runs, so header positions alone pin the emission order.
    // ------------------------------------------------------------------
    {
        UBlueprint* OrderBP = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
        TestNotNull(TEXT("ordering fixture Blueprint created"), OrderBP);
        if (OrderBP)
        {
            auto MakeGraph = [OrderBP](const TCHAR* Name) -> UEdGraph*
            {
                return FBlueprintEditorUtils::CreateNewGraph(
                    OrderBP, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            };

            UEdGraph* UberGraph = MakeGraph(TEXT("EventGraph"));
            // Registered Z-before-A on purpose: only SortAndAppend's Sorted.Sort can
            // recover alphabetical order from this.
            UEdGraph* ZFn    = MakeGraph(TEXT("ZFn"));
            UEdGraph* AFn    = MakeGraph(TEXT("AFn"));
            UEdGraph* ZMacro = MakeGraph(TEXT("ZMacro"));
            UEdGraph* AMacro = MakeGraph(TEXT("AMacro"));

            TestNotNull(TEXT("ordering fixture ubergraph created"), UberGraph);
            TestNotNull(TEXT("ordering fixture ZFn created"), ZFn);
            TestNotNull(TEXT("ordering fixture AFn created"), AFn);
            TestNotNull(TEXT("ordering fixture ZMacro created"), ZMacro);
            TestNotNull(TEXT("ordering fixture AMacro created"), AMacro);

            if (UberGraph && ZFn && AFn && ZMacro && AMacro)
            {
                OrderBP->UbergraphPages.Add(UberGraph);
                OrderBP->FunctionGraphs.Add(ZFn);
                OrderBP->FunctionGraphs.Add(AFn);
                OrderBP->MacroGraphs.Add(ZMacro);
                OrderBP->MacroGraphs.Add(AMacro);

                const FString OrderOutput = AssetDumpBuilder::BuildBpirText(OrderBP);

                const int32 UberIdx   = OrderOutput.Find(TEXT("# ==== Graph: EventGraph (ubergraph) ===="));
                const int32 AFnIdx    = OrderOutput.Find(TEXT("# ==== Graph: AFn (function) ===="));
                const int32 ZFnIdx    = OrderOutput.Find(TEXT("# ==== Graph: ZFn (function) ===="));
                const int32 AMacroIdx = OrderOutput.Find(TEXT("# ==== Graph: AMacro (macro) ===="));
                const int32 ZMacroIdx = OrderOutput.Find(TEXT("# ==== Graph: ZMacro (macro) ===="));

                TestTrue(TEXT("ubergraph header emitted"), UberIdx != INDEX_NONE);
                TestTrue(TEXT("AFn header emitted"), AFnIdx != INDEX_NONE);
                TestTrue(TEXT("ZFn header emitted"), ZFnIdx != INDEX_NONE);
                TestTrue(TEXT("AMacro header emitted"), AMacroIdx != INDEX_NONE);
                TestTrue(TEXT("ZMacro header emitted"), ZMacroIdx != INDEX_NONE);

                if (UberIdx != INDEX_NONE && AFnIdx != INDEX_NONE && ZFnIdx != INDEX_NONE
                    && AMacroIdx != INDEX_NONE && ZMacroIdx != INDEX_NONE)
                {
                    // AssetDumpBuilder.cpp:232 — SortAndAppend alpha-sorts each kind.
                    TestTrue(TEXT("function graphs are alpha-sorted (AFn before ZFn)"),
                        AFnIdx < ZFnIdx);
                    TestTrue(TEXT("macro graphs are alpha-sorted (AMacro before ZMacro)"),
                        AMacroIdx < ZMacroIdx);
                    // AssetDumpBuilder.cpp:224-227 before :242-243 — ubergraph pages first.
                    TestTrue(TEXT("ubergraph is emitted before every function graph"),
                        UberIdx < AFnIdx && UberIdx < ZFnIdx);
                    TestTrue(TEXT("ubergraph is emitted before every macro graph"),
                        UberIdx < AMacroIdx && UberIdx < ZMacroIdx);
                    // AssetDumpBuilder.cpp:242 before :243 — functions before macros.
                    TestTrue(TEXT("function graphs are emitted before macro graphs"),
                        ZFnIdx < AMacroIdx);
                }
            }
        }
    }

    // Try to load a known engine Blueprint that has an EventGraph.
    // /Engine/EngineSky/BP_Sky_Sphere is reliably present on all UE installs.
    const FString TestBpPath = TEXT("/Engine/EngineSky/BP_Sky_Sphere.BP_Sky_Sphere");
    UObject* Loaded = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *TestBpPath);
    UBlueprint* BP = Cast<UBlueprint>(Loaded);

    if (!BP)
    {
        // Asset not present in this project configuration — skip the graph-content check.
        // AddWarning, not AddInfo: a silently-skipped assertion block reads as a pass in
        // both the suite count and the log, which is exactly how missing coverage hides.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("BP_Sky_Sphere not loadable; skipping BpirConcatOrder graph-content checks."));
        return true;
    }

    FString Output = AssetDumpBuilder::BuildBpirText(BP);

    // If the blueprint has any graphs, output must begin with the graph header.
    const bool bHasGraphs =
        BP->UbergraphPages.Num() > 0 ||
        BP->FunctionGraphs.Num() > 0 ||
        BP->MacroGraphs.Num() > 0;

    if (bHasGraphs)
    {
        TestTrue(TEXT("Output starts with graph header"),
            Output.StartsWith(TEXT("# ==== Graph: ")));

        // Ubergraph pages come first; if any exist, kind must be "ubergraph".
        if (BP->UbergraphPages.Num() > 0)
        {
            TestTrue(TEXT("First graph block contains (ubergraph)"),
                Output.Contains(TEXT("(ubergraph)")));
        }
    }

    return true;
}

// ============================================================================
// AssetDumpBuilder.WidgetXmlOverriddenOnly
// Verifies null safety and that BuildOverriddenWidgetXml delegates to
// WidgetXmlExporter::BuildWidgetTreeXml with bIncludeDefaults=false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderWidgetXmlOverriddenOnlyTest,
    "PinWright.utils.asset_dump_builder.WidgetXmlOverriddenOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderWidgetXmlOverriddenOnlyTest::RunTest(const FString& Parameters)
{
    // Null safety: both entry points must return empty without crash.
    FString FromBuilder  = AssetDumpBuilder::BuildOverriddenWidgetXml(nullptr);
    FString FromExporter = WidgetXmlExporter::BuildWidgetTreeXml(nullptr, false);

    TestTrue(TEXT("BuildOverriddenWidgetXml(nullptr) returns empty"), FromBuilder.IsEmpty());
    TestTrue(TEXT("BuildWidgetTreeXml(nullptr, false) returns empty"), FromExporter.IsEmpty());

    // Include-defaults variant also survives null.
    FString FromExporterDefaults = WidgetXmlExporter::BuildWidgetTreeXml(nullptr, true);
    TestTrue(TEXT("BuildWidgetTreeXml(nullptr, true) returns empty"),
        FromExporterDefaults.IsEmpty());

    return true;
}

// ============================================================================
// AssetDumpBuilder.ScsJsonSkipsEmpty
// BuildScsJson on nullptr and on a Blueprint with no SCS nodes must return nullptr.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderScsJsonSkipsEmptyTest,
    "PinWright.utils.asset_dump_builder.ScsJsonSkipsEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderScsJsonSkipsEmptyTest::RunTest(const FString& Parameters)
{
    // Null blueprint must return nullptr.
    TSharedPtr<FJsonObject> NullResult = AssetDumpBuilder::BuildScsJson(nullptr);
    TestNull(TEXT("BuildScsJson(nullptr) returns nullptr"), NullResult.Get());

    // Load a pure data-only Blueprint that has no SCS nodes.
    // /Engine/EngineMaterials/DefaultMaterial is not a BP. Try BP_Sky_Sphere —
    // it has SCS nodes, so it's useful for the non-empty path.
    // For the "no SCS" path, we use a freshly constructed transient Blueprint
    // whose SimpleConstructionScript has no nodes.
    UBlueprint* TransientBP = NewObject<UBlueprint>(GetTransientPackage());
    TestNotNull(TEXT("Transient Blueprint created"), TransientBP);
    if (!TransientBP) return false;

    // A freshly created UBlueprint has no SCS at all (SimpleConstructionScript is null).
    TSharedPtr<FJsonObject> EmptyResult = AssetDumpBuilder::BuildScsJson(TransientBP);
    TestNull(TEXT("Transient BP with no SCS returns nullptr"), EmptyResult.Get());

    return true;
}

// ============================================================================
// WidgetXmlExporter.EmptyReason
// BuildWidgetTreeXmlWithDiagnostic must return a distinct Reason for each of the
// three null cases, and set bEmptyByDesign correctly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetXmlExporterEmptyReasonTest,
    "PinWright.handlers.widget_xml_exporter.EmptyReason",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetXmlExporterEmptyReasonTest::RunTest(const FString& Parameters)
{
    // Case 1: null WBP.
    {
        WidgetXmlExporter::FWidgetTreeXmlResult R =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(nullptr, false);
        TestTrue(TEXT("null WBP: Xml is empty"), R.Xml.IsEmpty());
        TestFalse(TEXT("null WBP: bEmptyByDesign is false"), R.bEmptyByDesign);
        TestFalse(TEXT("null WBP: Reason is non-empty"), R.Reason.IsEmpty());
    }

    // Case 2: WBP with null WidgetTree.
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        TestNotNull(TEXT("WBP for null-WidgetTree case created"), WBP);
        if (!WBP) return false;
        // WidgetTree is null by default on a bare NewObject<UWidgetBlueprint>.
        WBP->WidgetTree = nullptr;

        WidgetXmlExporter::FWidgetTreeXmlResult R =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(WBP, false);
        TestTrue(TEXT("null WidgetTree: Xml is empty"), R.Xml.IsEmpty());
        TestFalse(TEXT("null WidgetTree: bEmptyByDesign is false"), R.bEmptyByDesign);
        TestFalse(TEXT("null WidgetTree: Reason is non-empty"), R.Reason.IsEmpty());
    }

    // Case 3: WBP with allocated WidgetTree but null RootWidget.
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        TestNotNull(TEXT("WBP for null-RootWidget case created"), WBP);
        if (!WBP) return false;
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        // RootWidget is null by default.

        WidgetXmlExporter::FWidgetTreeXmlResult R =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(WBP, false);
        TestTrue(TEXT("null RootWidget: Xml is empty"), R.Xml.IsEmpty());
        TestTrue(TEXT("null RootWidget: bEmptyByDesign is true"), R.bEmptyByDesign);
        TestFalse(TEXT("null RootWidget: Reason is non-empty"), R.Reason.IsEmpty());
    }

    // All three Reason strings must be distinct.
    {
        WidgetXmlExporter::FWidgetTreeXmlResult R1 =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(nullptr, false);

        UWidgetBlueprint* WBP2 = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        if (!WBP2) return false;
        WBP2->WidgetTree = nullptr;
        WidgetXmlExporter::FWidgetTreeXmlResult R2 =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(WBP2, false);

        UWidgetBlueprint* WBP3 = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        if (!WBP3) return false;
        WBP3->WidgetTree = NewObject<UWidgetTree>(WBP3, NAME_None, RF_Transient);
        WidgetXmlExporter::FWidgetTreeXmlResult R3 =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(WBP3, false);

        TestTrue(TEXT("Reason(null WBP) != Reason(null WidgetTree)"), R1.Reason != R2.Reason);
        TestTrue(TEXT("Reason(null WBP) != Reason(null RootWidget)"), R1.Reason != R3.Reason);
        TestTrue(TEXT("Reason(null WidgetTree) != Reason(null RootWidget)"), R2.Reason != R3.Reason);
    }

    // Counterfactual: a WBP with a real RootWidget returns non-empty Xml and bEmptyByDesign=false.
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        if (!WBP) return false;
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("Root"));
        WBP->WidgetTree->RootWidget = Root;

        WidgetXmlExporter::FWidgetTreeXmlResult R =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(WBP, false);
        TestFalse(TEXT("real RootWidget: Xml is non-empty"), R.Xml.IsEmpty());
        TestFalse(TEXT("real RootWidget: bEmptyByDesign is false"), R.bEmptyByDesign);
        TestTrue(TEXT("real RootWidget: Reason is empty"), R.Reason.IsEmpty());
    }

    return true;
}

// ============================================================================
// AssetDumpHandler.handlers.asset_dump.WidgetTreeEmptyDiagnostic
// BuildWidgetTreeAspect_Internal with a WBP whose WidgetTree has no RootWidget
// must produce either a marker tree.xml entry in Files or a tree.xml entry in
// OutFileErrors — never neither. A WBP with a real RootWidget must produce a
// populated tree.xml and no tree.xml entry in OutFileErrors.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerWidgetTreeEmptyDiagnosticTest,
    "PinWright.handlers.asset_dump.WidgetTreeEmptyDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerWidgetTreeEmptyDiagnosticTest::RunTest(const FString& Parameters)
{
    // --- Case A: WBP with WidgetTree but null RootWidget (bEmptyByDesign path) ---
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        TestNotNull(TEXT("WBP for null-RootWidget case created"), WBP);
        if (!WBP) return false;
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        // RootWidget is null by default.

        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TPair<FString, FString>> FileErrors;
        AssetDumpHandler::BuildWidgetTreeAspect_Internal(WBP, Files, &FileErrors);

        const bool bHasTreeXmlFile = Files.ContainsByPredicate(
            [](const AssetDumpWriter::FDumpFile& F){ return F.Name == TEXT("tree.xml"); });
        const bool bHasTreeXmlError = FileErrors.ContainsByPredicate(
            [](const TPair<FString, FString>& E){ return E.Key == TEXT("tree.xml"); });

        TestTrue(TEXT("null RootWidget: tree.xml present as file OR diagnostic"),
            bHasTreeXmlFile || bHasTreeXmlError);

        // The null-RootWidget case is bEmptyByDesign — expect a marker file, not a diagnostic.
        TestTrue(TEXT("null RootWidget: marker tree.xml written (bEmptyByDesign path)"),
            bHasTreeXmlFile);
        TestFalse(TEXT("null RootWidget: no diagnostic entry written"), bHasTreeXmlError);

        if (bHasTreeXmlFile)
        {
            // The marker must be valid XML (comment node).
            const AssetDumpWriter::FDumpFile* TreeFile = Files.FindByPredicate(
                [](const AssetDumpWriter::FDumpFile& F){ return F.Name == TEXT("tree.xml"); });
            TestTrue(TEXT("marker tree.xml contains XML comment"),
                TreeFile && TreeFile->Content.Contains(TEXT("<!--")));
        }
    }

    // --- Case B: WBP with null WidgetTree (anomaly path → diagnostic) ---
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        if (!WBP) return false;
        WBP->WidgetTree = nullptr;

        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TPair<FString, FString>> FileErrors;
        AssetDumpHandler::BuildWidgetTreeAspect_Internal(WBP, Files, &FileErrors);

        const bool bHasTreeXmlFile = Files.ContainsByPredicate(
            [](const AssetDumpWriter::FDumpFile& F){ return F.Name == TEXT("tree.xml"); });
        const bool bHasTreeXmlError = FileErrors.ContainsByPredicate(
            [](const TPair<FString, FString>& E){ return E.Key == TEXT("tree.xml"); });

        TestTrue(TEXT("null WidgetTree: tree.xml present as file OR diagnostic"),
            bHasTreeXmlFile || bHasTreeXmlError);

        // null WidgetTree is an anomaly — expect a diagnostic, not a marker file.
        TestTrue(TEXT("null WidgetTree: diagnostic entry written"), bHasTreeXmlError);
        TestFalse(TEXT("null WidgetTree: no marker file written"), bHasTreeXmlFile);

        if (bHasTreeXmlError)
        {
            const TPair<FString, FString>* Err = FileErrors.FindByPredicate(
                [](const TPair<FString, FString>& E){ return E.Key == TEXT("tree.xml"); });
            TestTrue(TEXT("diagnostic reason is non-empty"),
                Err && !Err->Value.IsEmpty());
        }
    }

    // --- Counterfactual: WBP with a real RootWidget produces populated tree.xml,
    //     zero tree.xml errors ---
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        if (!WBP) return false;
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("Root"));
        WBP->WidgetTree->RootWidget = Root;

        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TPair<FString, FString>> FileErrors;
        AssetDumpHandler::BuildWidgetTreeAspect_Internal(WBP, Files, &FileErrors);

        const AssetDumpWriter::FDumpFile* TreeFile = Files.FindByPredicate(
            [](const AssetDumpWriter::FDumpFile& F){ return F.Name == TEXT("tree.xml"); });
        const bool bHasTreeXmlError = FileErrors.ContainsByPredicate(
            [](const TPair<FString, FString>& E){ return E.Key == TEXT("tree.xml"); });

        TestNotNull(TEXT("real RootWidget: tree.xml present in Files"), TreeFile);
        TestTrue(TEXT("real RootWidget: tree.xml content is non-empty"),
            TreeFile && !TreeFile->Content.IsEmpty());
        TestFalse(TEXT("real RootWidget: no tree.xml diagnostic entry"), bHasTreeXmlError);
    }

    return true;
}

// ============================================================================
// AssetDumpBuilder.PropertiesSkippedOnNullGeneratedClass
// Regression for B-asset-dump-properties-skipped-on-stub-class: when a Blueprint
// has no GeneratedClass (e.g. failed to compile), the dump must omit
// properties.json and record the explicit reason in meta.json propertiesStatus.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderPropertiesSkippedOnNullGeneratedClassTest,
    "PinWright.utils.asset_dump_builder.PropertiesSkippedOnNullGeneratedClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderPropertiesSkippedOnNullGeneratedClassTest::RunTest(const FString& Parameters)
{
    auto AssertPropertiesSkippedAndDiagnosed = [this](
        const TCHAR* CaseLabel,
        UBlueprint* BP)
    {
        // Sanity: the test must drive the helper with a stub-class scenario.
        TestNotNull(*FString::Printf(TEXT("%s: Blueprint created"), CaseLabel), BP);
        if (!BP) return;
        TestNull(*FString::Printf(TEXT("%s: GeneratedClass is null (stub-class scenario)"), CaseLabel),
            BP->GeneratedClass);

        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TPair<FString, FString>> Errors;
        AssetDumpHandler::FBlueprintPropertiesAspectStatus Status;
        AssetDumpHandler::BuildBlueprintPropertiesAspect_Internal(BP, Files, &Errors, &Status);

        const AssetDumpWriter::FDumpFile* PropsFile = Files.FindByPredicate(
            [](const AssetDumpWriter::FDumpFile& F){ return F.Name == DumpFileNames::Properties; });
        TestNull(*FString::Printf(TEXT("%s: properties.json absent from Files"), CaseLabel),
            PropsFile);
        TestEqual(*FString::Printf(TEXT("%s: propertiesStatus status"), CaseLabel),
            Status.Status, FString(TEXT("error")));
        TestEqual(*FString::Printf(TEXT("%s: propertiesStatus reason"), CaseLabel),
            Status.Reason, FString(TEXT("generated_class_missing")));
        TestEqual(*FString::Printf(TEXT("%s: property count is zero"), CaseLabel),
            Status.PropertyCount, 0);

        // A matching diagnostic must be recorded for properties.json.
        const TPair<FString, FString>* Err = Errors.FindByPredicate(
            [](const TPair<FString, FString>& E){ return E.Key == DumpFileNames::Properties; });
        TestNotNull(*FString::Printf(TEXT("%s: diagnostic for properties.json recorded"), CaseLabel),
            Err);
        if (Err)
        {
            TestFalse(*FString::Printf(TEXT("%s: diagnostic reason is non-empty"), CaseLabel),
                Err->Value.IsEmpty());
        }
    };

    // Case A: plain UBlueprint with no GeneratedClass.
    {
        UBlueprint* BP = NewObject<UBlueprint>(GetTransientPackage());
        // Explicitly skip FKismetEditorUtilities::CompileBlueprint — leave GeneratedClass null
        // to mimic the production failure mode (asset loaded but class failed to load).
        BP->GeneratedClass = nullptr;
        AssertPropertiesSkippedAndDiagnosed(TEXT("UBlueprint"), BP);
    }

    // Case B: UWidgetBlueprint with no GeneratedClass — symmetric path through the same helper.
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            NAME_None, RF_Transient);
        WBP->GeneratedClass = nullptr;
        AssertPropertiesSkippedAndDiagnosed(TEXT("UWidgetBlueprint"), WBP);
    }

    return true;
}

// ============================================================================
// AssetDumpBuilder.BlueprintEmptyPropertiesStatus
// Empty properties.json stays a pure property-name map; meta.json.propertiesStatus
// carries why it is empty. Counterfactual: if the AssetDumpHandler.cpp status
// annotation is reverted, this test fails because the production helper still
// emits {} but no durable status object exists to distinguish intentional empty
// from placeholder/error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderBlueprintEmptyPropertiesStatusTest,
    "PinWright.utils.asset_dump_builder.BlueprintEmptyPropertiesStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderBlueprintEmptyPropertiesStatusTest::RunTest(const FString& Parameters)
{
    auto AssertEmptyPropertiesStatus = [this](
        const TCHAR* CaseLabel,
        EBlueprintType BlueprintType,
        UClass* GeneratedClass,
        const FString& ExpectedStatus,
        const FString& ExpectedReason,
        const FString& ExpectedBlueprintType)
    {
        UBlueprint* BP = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
        TestNotNull(*FString::Printf(TEXT("%s: Blueprint created"), CaseLabel), BP);
        if (!BP) return;

        BP->BlueprintType = BlueprintType;
        BP->GeneratedClass = GeneratedClass;

        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TPair<FString, FString>> Errors;
        AssetDumpHandler::FBlueprintPropertiesAspectStatus Status;
        AssetDumpHandler::BuildBlueprintPropertiesAspect_Internal(BP, Files, &Errors, &Status);

        const AssetDumpWriter::FDumpFile* PropsFile = Files.FindByPredicate(
            [](const AssetDumpWriter::FDumpFile& F){ return F.Name == DumpFileNames::Properties; });
        if (GeneratedClass)
        {
            TestNotNull(*FString::Printf(TEXT("%s: properties.json present"), CaseLabel), PropsFile);
            if (!PropsFile) return;

            FString Compact = PropsFile->Content;
            Compact.ReplaceInline(TEXT("\r"), TEXT(""));
            Compact.ReplaceInline(TEXT("\n"), TEXT(""));
            Compact.ReplaceInline(TEXT("\t"), TEXT(""));
            Compact.ReplaceInline(TEXT(" "), TEXT(""));
            TestEqual(*FString::Printf(TEXT("%s: properties.json remains empty object"), CaseLabel),
                Compact, FString(TEXT("{}")));
        }
        else
        {
            TestNull(*FString::Printf(TEXT("%s: properties.json absent"), CaseLabel), PropsFile);
        }

        TSharedPtr<FJsonObject> Meta = AssetDumpBuilder::BuildMetaJson(BP);
        AssetDumpHandler::AttachBlueprintPropertiesStatusToMeta(Meta, Status);
        TestTrue(*FString::Printf(TEXT("%s: meta.json has propertiesStatus"), CaseLabel),
            Meta.IsValid() && Meta->HasTypedField<EJson::Object>(TEXT("propertiesStatus")));
        if (!Meta.IsValid() || !Meta->HasTypedField<EJson::Object>(TEXT("propertiesStatus"))) return;

        TSharedPtr<FJsonObject> StatusJson = Meta->GetObjectField(TEXT("propertiesStatus"));
        TestEqual(*FString::Printf(TEXT("%s: status"), CaseLabel),
            StatusJson->GetStringField(TEXT("status")), ExpectedStatus);
        TestEqual(*FString::Printf(TEXT("%s: reason"), CaseLabel),
            StatusJson->GetStringField(TEXT("reason")), ExpectedReason);
        if (!ExpectedBlueprintType.IsEmpty())
        {
            TestEqual(*FString::Printf(TEXT("%s: blueprintType"), CaseLabel),
                StatusJson->GetStringField(TEXT("blueprintType")), ExpectedBlueprintType);
        }
    };

    AssertEmptyPropertiesStatus(TEXT("normal no-overrides"),
        BPTYPE_Normal,
        UBlueprintFunctionLibrary::StaticClass(),
        TEXT("empty"),
        TEXT("no_overrides"),
        FString());
    AssertEmptyPropertiesStatus(TEXT("function library"),
        BPTYPE_FunctionLibrary,
        UBlueprintFunctionLibrary::StaticClass(),
        TEXT("empty"),
        TEXT("no_uproperties_on_blueprint_type"),
        TEXT("FunctionLibrary"));
    AssertEmptyPropertiesStatus(TEXT("macro library"),
        BPTYPE_MacroLibrary,
        UBlueprintFunctionLibrary::StaticClass(),
        TEXT("empty"),
        TEXT("no_uproperties_on_blueprint_type"),
        TEXT("MacroLibrary"));
    AssertEmptyPropertiesStatus(TEXT("interface"),
        BPTYPE_Interface,
        UInterface::StaticClass(),
        TEXT("empty"),
        TEXT("no_uproperties_on_blueprint_type"),
        TEXT("Interface"));
    AssertEmptyPropertiesStatus(TEXT("null GeneratedClass"),
        BPTYPE_Normal,
        nullptr,
        TEXT("error"),
        TEXT("generated_class_missing"),
        FString());

    return true;
}

// Regression: BP/WBP with null GeneratedClass must still report {Name}_C in className.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpBuilderMetaJsonBpStubClassTest,
    "PinWright.utils.asset_dump_builder.BpMetaGenClassNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpBuilderMetaJsonBpStubClassTest::RunTest(const FString& Parameters)
{
    // Case A: UBlueprint with null GeneratedClass and null ParentClass.
    {
        UBlueprint* BP = NewObject<UBlueprint>(GetTransientPackage(), TEXT("MyTestBP"));
        TestNotNull(TEXT("BP created"), BP);
        if (!BP) return false;
        // Defensively force the stub-class state — NewObject may leave these set.
        BP->GeneratedClass = nullptr;
        BP->ParentClass    = nullptr;

        TSharedPtr<FJsonObject> Meta = AssetDumpBuilder::BuildMetaJson(BP);
        TestNotNull(TEXT("BP: Meta JSON is not null"), Meta.Get());
        if (!Meta.IsValid()) return false;

        const FString ClassName   = Meta->GetStringField(TEXT("className"));
        const FString ParentClass = Meta->GetStringField(TEXT("parentClass"));

        TestEqual(TEXT("BP: className is synthesized {Name}_C"),
            ClassName, FString(TEXT("MyTestBP_C")));
        TestNotEqual(TEXT("BP: className is not the literal type name 'Blueprint'"),
            ClassName, FString(TEXT("Blueprint")));
        TestEqual(TEXT("BP: parentClass is empty when ParentClass is null"),
            ParentClass, FString());
    }

    // Case B: UWidgetBlueprint with null GeneratedClass and null ParentClass.
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(GetTransientPackage(),
            TEXT("MyTestWBP"));
        TestNotNull(TEXT("WBP created"), WBP);
        if (!WBP) return false;
        WBP->GeneratedClass = nullptr;
        WBP->ParentClass    = nullptr;

        TSharedPtr<FJsonObject> Meta = AssetDumpBuilder::BuildMetaJson(WBP);
        TestNotNull(TEXT("WBP: Meta JSON is not null"), Meta.Get());
        if (!Meta.IsValid()) return false;

        const FString ClassName   = Meta->GetStringField(TEXT("className"));
        const FString ParentClass = Meta->GetStringField(TEXT("parentClass"));

        TestEqual(TEXT("WBP: className is synthesized {Name}_C"),
            ClassName, FString(TEXT("MyTestWBP_C")));
        TestNotEqual(TEXT("WBP: className is not the literal type name 'WidgetBlueprint'"),
            ClassName, FString(TEXT("WidgetBlueprint")));
        TestEqual(TEXT("WBP: parentClass is empty when ParentClass is null"),
            ParentClass, FString());
    }

    return true;
}

// Regression: child WBP with null local RootWidget must walk the BPGC super-chain
// and emit the inherited tree annotated with inherited_from.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetXmlExporterInheritedRootTest,
    "PinWright.handlers.widget_xml_exporter.InheritedRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetXmlExporterInheritedRootTest::RunTest(const FString& Parameters)
{
    // Use FKismetEditorUtilities::CreateBlueprint with the WidgetBlueprint class types
    // rather than NewObject<UWidgetBlueprint>. A bare NewObject leaves SkeletonGeneratedClass,
    // SimpleConstructionScript, UbergraphPages, and the function graph stack uninitialized;
    // CompileBlueprint then crashes in FBlueprintEditorUtils::GetClassVariableList walking
    // a dangling FProperty list. CreateBlueprint is the canonical engine path
    // (see UMGEditor/WidgetBlueprintFactory.cpp:187) and produces a compile-safe instance.
    auto MakeFreshWBP = [](UClass* ParentClass) -> UWidgetBlueprint*
    {
        const FName UniqueName = MakeUniqueObjectName(
            GetTransientPackage(), UWidgetBlueprint::StaticClass(), TEXT("WBPInheritedRootFixture"));
        return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            ParentClass,
            GetTransientPackage(),
            UniqueName,
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("InheritedRootTest"))));
    };

    // --- Case A: child WBP inherits RootWidget from a compiled parent WBP ---
    {
        // Build the parent WBP with a CanvasPanel root named "ParentRoot".
        UWidgetBlueprint* ParentWBP = MakeFreshWBP(UUserWidget::StaticClass());
        TestNotNull(TEXT("Parent WBP created"), ParentWBP);
        if (!ParentWBP) return false;
        TestNotNull(TEXT("Parent WBP has WidgetTree from CreateBlueprint"), ParentWBP->WidgetTree.Get());
        if (!ParentWBP->WidgetTree) return false;

        UCanvasPanel* ParentRoot = ParentWBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("ParentRoot"));
        ParentWBP->WidgetTree->RootWidget = ParentRoot;

        // Compile the parent so GeneratedClass is a real UWidgetBlueprintGeneratedClass
        // with a populated WidgetTree archetype; required for FindWidgetTreeOwningClass().
        FKismetEditorUtilities::CompileBlueprint(ParentWBP);
        TestNotNull(TEXT("Parent WBP compiled GeneratedClass"), ParentWBP->GeneratedClass.Get());
        if (!ParentWBP->GeneratedClass) return false;

        UWidgetBlueprintGeneratedClass* ParentBPGC =
            Cast<UWidgetBlueprintGeneratedClass>(ParentWBP->GeneratedClass);
        TestNotNull(TEXT("Parent GeneratedClass is a WidgetBlueprintGeneratedClass"), ParentBPGC);
        if (!ParentBPGC) return false;

        // Build the child WBP whose ParentClass is the compiled parent's BPGC.
        UWidgetBlueprint* ChildWBP = MakeFreshWBP(ParentBPGC);
        TestNotNull(TEXT("Child WBP created"), ChildWBP);
        if (!ChildWBP) return false;

        // CreateBlueprint may have populated a default root widget on the child; null it out
        // so RootWidget stays null and the inheritance walk is exercised.
        if (ChildWBP->WidgetTree)
        {
            ChildWBP->WidgetTree->RootWidget = nullptr;
        }
        FKismetEditorUtilities::CompileBlueprint(ChildWBP);

        WidgetXmlExporter::FWidgetTreeXmlResult R =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(ChildWBP, false);

        TestFalse(TEXT("inherited root: Xml is non-empty"), R.Xml.IsEmpty());
        TestFalse(TEXT("inherited root: bEmptyByDesign is false"), R.bEmptyByDesign);
        TestTrue(TEXT("inherited root: Xml contains parent root widget name"),
            R.Xml.Contains(TEXT("ParentRoot")));
        TestTrue(TEXT("inherited root: Xml contains inherited_from attribute"),
            R.Xml.Contains(TEXT("inherited_from=")));
        TestTrue(TEXT("inherited root: Xml contains parent path"),
            R.Xml.Contains(*ParentWBP->GetPathName()));
    }

    // --- Case B: child WBP with native UUserWidget parent (no inheritable RootWidget) ---
    {
        UWidgetBlueprint* ChildWBP = MakeFreshWBP(UUserWidget::StaticClass());
        TestNotNull(TEXT("Native-parent child WBP created"), ChildWBP);
        if (!ChildWBP) return false;

        // Force a null RootWidget to exercise the native-parent branch.
        if (ChildWBP->WidgetTree)
        {
            ChildWBP->WidgetTree->RootWidget = nullptr;
        }
        FKismetEditorUtilities::CompileBlueprint(ChildWBP);

        WidgetXmlExporter::FWidgetTreeXmlResult R2 =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(ChildWBP, false);

        TestTrue(TEXT("native parent: Xml is empty"), R2.Xml.IsEmpty());
        TestTrue(TEXT("native parent: bEmptyByDesign is true"), R2.bEmptyByDesign);
        TestTrue(TEXT("native parent: Reason names the inheritable-RootWidget shortfall"),
            R2.Reason.Contains(TEXT("inheritable")));
    }

    return true;
}
