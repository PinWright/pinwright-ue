// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-material-graph-no-transaction: every material.graph.* mutator opens
// exactly one FScopedTransaction (PinWright::Material::FScopedMaterialGraphEdit), so one
// GEditor->UndoTransaction reverses the whole call. Each test first asserts that the NEWEST undo
// entry is the verb's own titled transaction and only then undoes, so a reverted fix fails on that
// assertion instead of undoing some other test's transaction.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunction.h"

namespace TestMaterialGraphUndoHelpers
{
    // Flags a factory-created asset carries (UAssetToolsImpl::CreateAsset passes RF_Transactional);
    // the editor-only data inherits RF_Transactional through RF_PropagateToSubObjects.
    template <typename AssetType>
    AssetType* CreateTransactionalAsset(FAutomationTestBase& Test, const TCHAR* Prefix, FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/%s_%s"),
            Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* Package = CreatePackage(*OutPackagePath);
        if (!Test.TestNotNull(TEXT("fixture package created"), Package)) return nullptr;
        AssetType* Asset = NewObject<AssetType>(Package,
            FName(*FPackageName::GetLongPackageAssetName(OutPackagePath)),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Test.TestNotNull(TEXT("fixture asset created"), Asset))
        {
            CleanupTestAsset(OutPackagePath);
            return nullptr;
        }
        FAssetRegistryModule::AssetCreated(Asset);
        return Asset;
    }

    template <typename OwnerType>
    UMaterialExpression* AddExpression(FAutomationTestBase& Test, OwnerType* Owner, UClass* ExpressionClass)
    {
        const FCreateResult Result = FMaterialExpressionFactory::Create(Owner, ExpressionClass, nullptr, FVector2D(0.0, 0.0));
        if (!Test.TestTrue(TEXT("fixture expression created through the production factory"), Result.IsSuccess()))
        {
            return nullptr;
        }
        if (!Result.Expression->MaterialExpressionGuid.IsValid())
        {
            Result.Expression->MaterialExpressionGuid = FGuid::NewGuid();
        }
        return Result.Expression;
    }

    int32 CountOf(const UMaterial* Material)
    {
        return Material && Material->GetEditorOnlyData() ? Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Num() : -1;
    }

    int32 CountOf(const UMaterialFunction* Function)
    {
        return Function && Function->GetEditorOnlyData() ? Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Num() : -1;
    }

    bool Invoke(FAutomationTestBase& Test, const TCHAR* Method, const TSharedPtr<FJsonObject>& Payload)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s is registered"), Method), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s succeeds (%s: %s)"), Method, *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        return bFound && Capture.bSuccess;
    }

    // The precondition for every undo below: the newest entry is this verb's one transaction and it
    // recorded MustContain. Without it a reverted fix would undo whatever an earlier test left there.
    bool NewestUndoIs(FAutomationTestBase& Test, const TCHAR* Method, const UObject* MustContain)
    {
        if (!Test.TestTrue(TEXT("editor transaction buffer exists"), GEditor && GEditor->Trans)) return false;
        const int32 Newest = GEditor->Trans->GetQueueLength() - 1;
        const FTransaction* Transaction = Newest >= 0 ? GEditor->Trans->GetTransaction(Newest) : nullptr;
        if (!Test.TestNotNull(*FString::Printf(TEXT("%s recorded an undo transaction"), Method), Transaction)) return false;
        const FString Expected = FString::Printf(TEXT("PinWright: %s"), Method);
        const bool bTitle = Test.TestEqual(*FString::Printf(TEXT("newest undo entry is %s's own"), Method),
            Transaction->GetContext().Title.ToString(), Expected);
        const bool bContains = Test.TestTrue(*FString::Printf(TEXT("%s's transaction recorded %s"), Method, *GetNameSafe(MustContain)),
            Transaction->ContainsObject(MustContain));
        return bTitle && bContains;
    }

    bool Undo(FAutomationTestBase& Test) { return Test.TestTrue(TEXT("undo succeeds"), GEditor->UndoTransaction()); }
    bool Redo(FAutomationTestBase& Test) { return Test.TestTrue(TEXT("redo succeeds"), GEditor->RedoTransaction()); }

    TSharedPtr<FJsonObject> Payload(const TCHAR* PathKey, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(PathKey, AssetPath);
        return Obj;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphUndoConnectNodesTest,
    "PinWright.material.graph.undo.ConnectNodesUndoesAndRedoes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphUndoConnectNodesTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialGraphUndoHelpers;
    FString PackagePath;
    UMaterial* Material = CreateTransactionalAsset<UMaterial>(*this, TEXT("UndoConnect"), PackagePath);
    if (!Material) return true;
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UMaterialExpression* Source = AddExpression(*this, Material, UMaterialExpressionScalarParameter::StaticClass());
    UMaterialExpressionMultiply* Target = Cast<UMaterialExpressionMultiply>(
        AddExpression(*this, Material, UMaterialExpressionMultiply::StaticClass()));
    if (!Source || !Target || !Material->GetEditorOnlyData()) return true;
    TestTrue(TEXT("fixture material is transactional"), Material->HasAnyFlags(RF_Transactional)
        && Material->GetEditorOnlyData()->HasAnyFlags(RF_Transactional));
    TestNull(TEXT("fixture starts unwired"), Target->A.Expression);

    // Expression input.
    TSharedPtr<FJsonObject> Wire = Payload(TEXT("assetPath"), Material->GetPathName());
    Wire->SetStringField(TEXT("sourceNodeId"), Source->MaterialExpressionGuid.ToString());
    Wire->SetStringField(TEXT("targetNodeId"), Target->MaterialExpressionGuid.ToString());
    Wire->SetStringField(TEXT("inputName"), TEXT("A"));
    if (!Invoke(*this, TEXT("material.graph.connect_nodes"), Wire)) return true;
    TestTrue(TEXT("connect_nodes wired Multiply.A"), Target->A.Expression == Source);
    if (!NewestUndoIs(*this, TEXT("material.graph.connect_nodes"), Target)) return true;
    if (Undo(*this)) TestNull(TEXT("undo removes the wire"), Target->A.Expression);
    if (Redo(*this)) TestTrue(TEXT("redo restores the wire"), Target->A.Expression == Source);

    // Main-node input: lives on UMaterialEditorOnlyData, which UMaterial::Modify forwards to.
    TSharedPtr<FJsonObject> Main = Payload(TEXT("assetPath"), Material->GetPathName());
    Main->SetStringField(TEXT("sourceNodeId"), Source->MaterialExpressionGuid.ToString());
    Main->SetStringField(TEXT("targetNodeId"), TEXT("Main"));
    Main->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
    if (!Invoke(*this, TEXT("material.graph.connect_nodes"), Main)) return true;
    TestTrue(TEXT("connect_nodes wired BaseColor"), Material->GetEditorOnlyData()->BaseColor.Expression == Source);
    if (!NewestUndoIs(*this, TEXT("material.graph.connect_nodes"), Material->GetEditorOnlyData())) return true;
    if (Undo(*this)) TestNull(TEXT("undo clears BaseColor"), Material->GetEditorOnlyData()->BaseColor.Expression);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphUndoCreateNodesTest,
    "PinWright.material.graph.undo.CreateNodesBatchIsOneUndoStep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphUndoCreateNodesTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialGraphUndoHelpers;
    FString PackagePath;
    UMaterial* Material = CreateTransactionalAsset<UMaterial>(*this, TEXT("UndoCreateNodes"), PackagePath);
    if (!Material) return true;
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };
    const int32 Before = CountOf(Material);
    TestEqual(TEXT("fixture starts empty"), Before, 0);

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const TCHAR* Type : { TEXT("Constant"), TEXT("Add"), TEXT("ScalarParameter") })
    {
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("type"), Type);
        Node->SetNumberField(TEXT("x"), 0.0);
        Node->SetNumberField(TEXT("y"), 0.0);
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    TSharedPtr<FJsonObject> Batch = Payload(TEXT("materialPath"), Material->GetPathName());
    Batch->SetArrayField(TEXT("nodes"), Nodes);
    if (!Invoke(*this, TEXT("material.graph.create_nodes"), Batch)) return true;
    TestEqual(TEXT("create_nodes added three nodes"), CountOf(Material), Before + 3);

    if (!NewestUndoIs(*this, TEXT("material.graph.create_nodes"), Material->GetEditorOnlyData())) return true;
    if (Undo(*this)) TestEqual(TEXT("ONE undo removes the whole batch"), CountOf(Material), Before);
    if (Redo(*this)) TestEqual(TEXT("redo restores the whole batch"), CountOf(Material), Before + 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphUndoRemoveNodeTest,
    "PinWright.material.graph.undo.RemoveNodeRestoresConfiguredNodeAndWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphUndoRemoveNodeTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialGraphUndoHelpers;
    FString PackagePath;
    UMaterial* Material = CreateTransactionalAsset<UMaterial>(*this, TEXT("UndoRemoveNode"), PackagePath);
    if (!Material) return true;
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UMaterialExpressionScalarParameter* Victim = Cast<UMaterialExpressionScalarParameter>(
        AddExpression(*this, Material, UMaterialExpressionScalarParameter::StaticClass()));
    UMaterialExpressionMultiply* Survivor = Cast<UMaterialExpressionMultiply>(
        AddExpression(*this, Material, UMaterialExpressionMultiply::StaticClass()));
    if (!Victim || !Survivor || !Material->GetEditorOnlyData()) return true;
    Victim->ParameterName = FName(TEXT("ConfiguredParameter"));
    Victim->DefaultValue = 0.25f;
    Survivor->A.Expression = Victim;
    Material->GetEditorOnlyData()->BaseColor.Expression = Victim;
    const int32 Before = CountOf(Material);

    TSharedPtr<FJsonObject> Remove = Payload(TEXT("assetPath"), Material->GetPathName());
    Remove->SetStringField(TEXT("nodeId"), Victim->MaterialExpressionGuid.ToString());
    if (!Invoke(*this, TEXT("material.graph.remove_node"), Remove)) return true;
    TestEqual(TEXT("remove_node removed one node"), CountOf(Material), Before - 1);
    TestNull(TEXT("remove_node cleared the surviving wire"), Survivor->A.Expression);

    // The delete touches the victim, the sibling whose input it clears, and the editor-only data.
    if (!NewestUndoIs(*this, TEXT("material.graph.remove_node"), Victim)) return true;
    TestTrue(TEXT("remove_node's transaction recorded the sibling whose input it cleared"),
        GEditor->Trans->GetTransaction(GEditor->Trans->GetQueueLength() - 1)->ContainsObject(Survivor));
    if (Undo(*this))
    {
        TestEqual(TEXT("undo restores the node count"), CountOf(Material), Before);
        TestTrue(TEXT("undo puts the victim back in the collection"),
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Contains(Victim));
        TestTrue(TEXT("undo revives the victim"), IsValid(Victim));
        TestTrue(TEXT("undo keeps the victim's configured name"), Victim->ParameterName == FName(TEXT("ConfiguredParameter")));
        TestEqual(TEXT("undo keeps the victim's configured value"), Victim->DefaultValue, 0.25f);
        TestTrue(TEXT("undo restores the sibling wire"), Survivor->A.Expression == static_cast<UMaterialExpression*>(Victim));
        TestTrue(TEXT("undo restores the main-node wire"), Material->GetEditorOnlyData()->BaseColor.Expression == static_cast<UMaterialExpression*>(Victim));
    }
    if (Redo(*this))
    {
        TestEqual(TEXT("redo removes the node again"), CountOf(Material), Before - 1);
        TestNull(TEXT("redo clears the sibling wire again"), Survivor->A.Expression);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphUndoEveryMutatorTest,
    "PinWright.material.graph.undo.EveryMutatorRecordsItsOwnStep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphUndoEveryMutatorTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialGraphUndoHelpers;
    FString PackagePath;
    UMaterial* Material = CreateTransactionalAsset<UMaterial>(*this, TEXT("UndoEveryMutator"), PackagePath);
    if (!Material) return true;
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };
    FString FunctionPackagePath;
    UMaterialFunction* Function = CreateTransactionalAsset<UMaterialFunction>(*this, TEXT("UndoEveryMutatorFn"), FunctionPackagePath);
    if (!Function) return true;
    ON_SCOPE_EXIT { CleanupTestAsset(FunctionPackagePath); };
    if (!Material->GetEditorOnlyData() || !Function->GetEditorOnlyData()) return true;
    const FString MaterialPath = Material->GetPathName();

    // A refusal after the scope opened (property validation inside the factory) leaves no entry.
    {
        const int32 QueueBefore = GEditor->Trans->GetQueueLength();
        TSharedPtr<FJsonObject> Bad = Payload(TEXT("materialPath"), MaterialPath);
        Bad->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        Props->SetNumberField(TEXT("NoSuchProperty"), 1.0);
        Bad->SetObjectField(TEXT("properties"), Props);
        Bad->SetNumberField(TEXT("x"), 0.0);
        Bad->SetNumberField(TEXT("y"), 0.0);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.graph.add_expression"), Bad, Capture);
        TestFalse(TEXT("add_expression refuses an unknown property"), Capture.bSuccess);
        TestEqual(TEXT("a refused add_expression leaves no undo entry"), GEditor->Trans->GetQueueLength(), QueueBefore);
    }

    // add_node on a material, then on a material function (whose editor-only data
    // UMaterialFunction::Modify does NOT forward to).
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        UObject* Owner = Pass == 0 ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function);
        UObject* EditorOnly = Pass == 0 ? static_cast<UObject*>(Material->GetEditorOnlyData()) : static_cast<UObject*>(Function->GetEditorOnlyData());
        auto Count = [&]() { return Pass == 0 ? CountOf(Material) : CountOf(Function); };
        const int32 Before = Count();
        TSharedPtr<FJsonObject> Add = Payload(TEXT("assetPath"), Owner->GetPathName());
        Add->SetStringField(TEXT("nodeType"), TEXT("Constant"));
        Add->SetNumberField(TEXT("x"), 0.0);
        Add->SetNumberField(TEXT("y"), 0.0);
        if (!Invoke(*this, TEXT("material.graph.add_node"), Add)) return true;
        TestEqual(TEXT("add_node added one node"), Count(), Before + 1);
        if (!NewestUndoIs(*this, TEXT("material.graph.add_node"), EditorOnly)) return true;
        if (Undo(*this)) TestEqual(TEXT("undo removes the added node"), Count(), Before);
    }

    // add_expression
    {
        const int32 Before = CountOf(Material);
        TSharedPtr<FJsonObject> Add = Payload(TEXT("materialPath"), MaterialPath);
        Add->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
        Add->SetNumberField(TEXT("x"), 0.0);
        Add->SetNumberField(TEXT("y"), 0.0);
        if (!Invoke(*this, TEXT("material.graph.add_expression"), Add)) return true;
        if (!NewestUndoIs(*this, TEXT("material.graph.add_expression"), Material->GetEditorOnlyData())) return true;
        if (Undo(*this)) TestEqual(TEXT("undo removes the added expression"), CountOf(Material), Before);
    }

    // add_texture_sample
    {
        const int32 Before = CountOf(Material);
        TSharedPtr<FJsonObject> Add = Payload(TEXT("materialPath"), MaterialPath);
        Add->SetStringField(TEXT("texturePath"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
        Add->SetNumberField(TEXT("x"), 0.0);
        Add->SetNumberField(TEXT("y"), 0.0);
        if (!Invoke(*this, TEXT("material.graph.add_texture_sample"), Add)) return true;
        if (!NewestUndoIs(*this, TEXT("material.graph.add_texture_sample"), Material->GetEditorOnlyData())) return true;
        if (Undo(*this)) TestEqual(TEXT("undo removes the texture sample"), CountOf(Material), Before);
    }

    // break_connections: one expression pin, then one main-node pin.
    UMaterialExpression* Source = AddExpression(*this, Material, UMaterialExpressionScalarParameter::StaticClass());
    UMaterialExpressionMultiply* Target = Cast<UMaterialExpressionMultiply>(
        AddExpression(*this, Material, UMaterialExpressionMultiply::StaticClass()));
    if (!Source || !Target) return true;
    Target->A.Expression = Source;
    Material->GetEditorOnlyData()->Roughness.Expression = Source;
    {
        TSharedPtr<FJsonObject> Break = Payload(TEXT("assetPath"), MaterialPath);
        Break->SetStringField(TEXT("nodeId"), Target->MaterialExpressionGuid.ToString());
        if (!Invoke(*this, TEXT("material.graph.break_connections"), Break)) return true;
        TestNull(TEXT("break_connections cleared Multiply.A"), Target->A.Expression);
        if (!NewestUndoIs(*this, TEXT("material.graph.break_connections"), Target)) return true;
        if (Undo(*this)) TestTrue(TEXT("undo restores Multiply.A"), Target->A.Expression == Source);
    }
    {
        TSharedPtr<FJsonObject> Break = Payload(TEXT("assetPath"), MaterialPath);
        Break->SetStringField(TEXT("nodeId"), TEXT("Main"));
        Break->SetStringField(TEXT("pinName"), TEXT("Roughness"));
        if (!Invoke(*this, TEXT("material.graph.break_connections"), Break)) return true;
        TestNull(TEXT("break_connections cleared Roughness"), Material->GetEditorOnlyData()->Roughness.Expression);
        if (!NewestUndoIs(*this, TEXT("material.graph.break_connections"), Material->GetEditorOnlyData())) return true;
        if (Undo(*this)) TestTrue(TEXT("undo restores Roughness"), Material->GetEditorOnlyData()->Roughness.Expression == Source);
    }
    return true;
}

// The plugin's own creators (material.authoring.create_material, MGIR) pass only RF_Public |
// RF_Standalone, so the material and its editor-only data are NOT transactional. Recording must
// still capture the expression collection: otherwise undo of add_node leaves a dead entry and undo
// of remove_node revives the victim without putting it back in the collection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphUndoNonTransactionalAssetTest,
    "PinWright.material.graph.undo.NonTransactionalAssetUndoesCleanly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphUndoNonTransactionalAssetTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialGraphUndoHelpers;
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/UndoNonTransactional_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };
    UMaterial* Material = NewObject<UMaterial>(CreatePackage(*PackagePath),
        FName(*FPackageName::GetLongPackageAssetName(PackagePath)), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("fixture material created"), Material) || !Material->GetEditorOnlyData()) return true;
    FAssetRegistryModule::AssetCreated(Material);
    TestFalse(TEXT("precondition: the material is created without RF_Transactional"),
        Material->HasAnyFlags(RF_Transactional) || Material->GetEditorOnlyData()->HasAnyFlags(RF_Transactional));

    auto CollectionIsClean = [Material]()
    {
        for (UMaterialExpression* Expr : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
        {
            if (!IsValid(Expr)) return false;
        }
        return true;
    };

    // add_node, then undo: count restored, no null / garbage entry left behind.
    {
        const int32 Before = CountOf(Material);
        TSharedPtr<FJsonObject> Add = Payload(TEXT("assetPath"), Material->GetPathName());
        Add->SetStringField(TEXT("nodeType"), TEXT("Constant"));
        Add->SetNumberField(TEXT("x"), 0.0);
        Add->SetNumberField(TEXT("y"), 0.0);
        if (!Invoke(*this, TEXT("material.graph.add_node"), Add)) return true;
        if (!NewestUndoIs(*this, TEXT("material.graph.add_node"), Material->GetEditorOnlyData())) return true;
        if (Undo(*this))
        {
            TestEqual(TEXT("undo of add_node restores the node count"), CountOf(Material), Before);
            TestTrue(TEXT("undo of add_node leaves no null or garbage entry"), CollectionIsClean());
        }
    }

    // remove_node of a wired node, then undo: the victim is back IN the collection, not just revived.
    UMaterialExpression* Victim = AddExpression(*this, Material, UMaterialExpressionScalarParameter::StaticClass());
    UMaterialExpressionMultiply* Survivor = Cast<UMaterialExpressionMultiply>(
        AddExpression(*this, Material, UMaterialExpressionMultiply::StaticClass()));
    if (!Victim || !Survivor) return true;
    Victim->ClearFlags(RF_Transactional);
    Survivor->ClearFlags(RF_Transactional);
    Survivor->A.Expression = Victim;
    const int32 Before = CountOf(Material);
    TSharedPtr<FJsonObject> Remove = Payload(TEXT("assetPath"), Material->GetPathName());
    Remove->SetStringField(TEXT("nodeId"), Victim->MaterialExpressionGuid.ToString());
    if (!Invoke(*this, TEXT("material.graph.remove_node"), Remove)) return true;
    if (!NewestUndoIs(*this, TEXT("material.graph.remove_node"), Material->GetEditorOnlyData())) return true;
    if (Undo(*this))
    {
        TestEqual(TEXT("undo of remove_node restores the node count"), CountOf(Material), Before);
        TestTrue(TEXT("undo of remove_node puts the victim back in the collection"),
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Contains(Victim));
        TestTrue(TEXT("undo of remove_node revives the victim"), IsValid(Victim));
        TestTrue(TEXT("undo of remove_node restores the sibling wire"), Survivor->A.Expression == Victim);
        TestTrue(TEXT("the collection holds no null or garbage entry"), CollectionIsClean());
    }
    return true;
}
