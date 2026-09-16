// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-material-expression-missing-back-pointer.
//
// UMaterialExpression carries two serialized back-pointers, Material and Function, and the engine
// gates its own change-forwarding on them: UMaterialExpression::PostEditChangeProperty forwards an
// expression-level edit to Material->PostEditChangeProperty when Material is set, and falls through
// to Function->PostEditChangeProperty when it is not. Outering the node to the asset does NOT set
// them — the engine reads the fields, not GetOuter().
//
// Every PinWright create site used to leave both null, so a LATER write to any property of a
// PinWright-created node (property.set on Code, a typed setter, a hand edit) landed on the
// expression object and the owning asset never recompiled: a silent no-effect. The material editor
// hides this, because opening the asset runs its own RestoreExpressionBackReferences repair pass.
//
// These tests drive the four distinct creation code paths and assert the back-pointer resolves to
// the owning asset. Before the fix every back-pointer assertion below reads null and fails.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    // A persisted-package material with an EMPTY expression collection, so every expression present
    // at the end of a test demonstrably came from a PinWright verb.
    UMaterial* CreateBackPointerFixtureMaterial(
        FAutomationTestBase& Test, const TCHAR* Prefix, FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*OutPackagePath);
        if (!Test.TestNotNull(TEXT("Fixture package created"), Pkg))
            return nullptr;

        UMaterial* Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutPackagePath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Fixture material created"), Material))
        {
            CleanupTestAsset(OutPackagePath);
            return nullptr;
        }

        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    // Invoke a node-creating verb, assert it was registered and succeeded, and return its nodeId.
    FString InvokeCreateVerb(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), Method), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            Test.AddError(FString::Printf(TEXT("%s failed: %s — %s"),
                Method, *Capture.ErrorCode, *Capture.Message));
            return FString();
        }
        // TryGet, not GetStringField: the asset-create verbs reused here return no nodeId, and
        // GetStringField logs a LogJson warning for a missing field, which automation elevates
        // to a test error.
        FString NodeId;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
        }
        return NodeId;
    }

    UMaterialExpression* FindByGuid(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
        const FString& NodeId)
    {
        if (NodeId.IsEmpty())
            return nullptr;
        for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
        {
            if (Expr && Expr->MaterialExpressionGuid.ToString() == NodeId)
                return Expr.Get();
        }
        return nullptr;
    }

    // The assertion the ticket asks for, per node: the created expression exists and its owning
    // back-pointer is the asset that owns it — not null, and not some other material.
    void AssertMaterialBackPointer(FAutomationTestBase& Test, UMaterial* Material,
        const FString& NodeId, const TCHAR* Label)
    {
        UMaterialExpression* Expr = FindByGuid(Material->GetExpressions(), NodeId);
        if (!Test.TestNotNull(*FString::Printf(TEXT("%s node landed in the material"), Label), Expr))
            return;
        // Spelled as TestTrue + an explicit error: TestEqual on pointers reports only "the two
        // values are not equal", which cannot tell a null back-pointer (the defect) from one
        // pointing at the wrong material.
        const bool bResolves = static_cast<UMaterial*>(Expr->Material) == Material;
        Test.TestTrue(
            *FString::Printf(TEXT("%s node's Material back-pointer resolves to the owning material"), Label),
            bResolves);
        if (!bResolves)
        {
            Test.AddError(FString::Printf(
                TEXT("%s created a node whose Material back-pointer is %s; expected %s."),
                Label,
                Expr->Material ? *Expr->Material->GetPathName() : TEXT("null"),
                *Material->GetPathName()));
        }
    }
}


// Material graph: the three code paths that create a node inside a UMaterial — the factory
// (FMaterialExpressionFactory::Create, reached by most typed add_* verbs), the direct NewObject in
// MaterialAuthoringHandler's add_custom_expression, the FINALIZE_EXPR_AND_RESPOND path used by
// use_material_function, and the direct NewObject in MaterialGraphHandler's add_texture_sample.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialExpressionOwnerBackPointerMaterialTest,
    "PinWright.material.authoring.expression_back_pointer.CreatedExpressionsResolveOwningMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialExpressionOwnerBackPointerMaterialTest::RunTest(const FString& Parameters)
{
    FString MaterialPackagePath;
    UMaterial* Material = CreateBackPointerFixtureMaterial(*this, TEXT("BackPtrMat"), MaterialPackagePath);
    if (!Material)
        return true;

    const FString MaterialPath = Material->GetPathName();

    // A function to point use_material_function at, so the FINALIZE_EXPR_AND_RESPOND path is exercised.
    const FString FuncName = FString::Printf(TEXT("MF_BackPtr_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString FuncFolder = TEXT("/Game/__PW_GatewayTests");
    const FString FuncPackagePath = FString::Printf(TEXT("%s/%s"), *FuncFolder, *FuncName);
    const FString FuncObjectPath = FString::Printf(TEXT("%s.%s"), *FuncPackagePath, *FuncName);

    // One guard, material first: the material's FunctionCall node references the function, so
    // deleting the function while that reference is live makes the force-delete null it out.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(MaterialPackagePath);
        CleanupTestAsset(FuncPackagePath);
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FuncName);
        Payload->SetStringField(TEXT("path"), FuncFolder);
        Payload->SetBoolField(TEXT("save"), false);
        InvokeCreateVerb(*this, TEXT("material.authoring.create_material_function"), Payload);
    }
    {
        // Give it an output — a function-call node pointing at an output-less function is a
        // degenerate case the engine complains about, and it is not what this test is measuring.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Result"));
        Payload->SetNumberField(TEXT("x"), 200.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        InvokeCreateVerb(*this, TEXT("material.authoring.add_function_output"), Payload);
    }

    // 1. Direct NewObject in MaterialAuthoringHandler (the path the ticket reproduced on).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialPath);
        Payload->SetStringField(TEXT("code"), TEXT("return float3(1,0,0);"));
        Payload->SetStringField(TEXT("outputType"), TEXT("Float3"));
        Payload->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
        Payload->SetNumberField(TEXT("x"), -300.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        AssertMaterialBackPointer(*this, Material,
            InvokeCreateVerb(*this, TEXT("material.authoring.add_custom_expression"), Payload),
            TEXT("add_custom_expression"));
    }

    // 2. FMaterialExpressionFactory::Create — the shared path behind ~30 typed add_* verbs.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialPath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("BackPointerProbe"));
        Payload->SetNumberField(TEXT("x"), -300.0);
        Payload->SetNumberField(TEXT("y"), 200.0);
        AssertMaterialBackPointer(*this, Material,
            InvokeCreateVerb(*this, TEXT("material.authoring.add_scalar_parameter"), Payload),
            TEXT("add_scalar_parameter"));
    }

    // 3. FINALIZE_EXPR_AND_RESPOND (use_material_function).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialPath);
        Payload->SetStringField(TEXT("functionPath"), FuncObjectPath);
        Payload->SetNumberField(TEXT("x"), 0.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        AssertMaterialBackPointer(*this, Material,
            InvokeCreateVerb(*this, TEXT("material.authoring.use_material_function"), Payload),
            TEXT("use_material_function"));
    }

    // 4. Direct NewObject in MaterialGraphHandler (add_texture_sample).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("materialPath"), MaterialPath);
        Payload->SetStringField(TEXT("texturePath"),
            TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
        Payload->SetNumberField(TEXT("x"), -300.0);
        Payload->SetNumberField(TEXT("y"), 400.0);
        AssertMaterialBackPointer(*this, Material,
            InvokeCreateVerb(*this, TEXT("material.graph.add_texture_sample"), Payload),
            TEXT("material.graph.add_texture_sample"));
    }

    // Defect-class sweep: the fixture started empty, so every expression here came from a verb.
    // A future create site that forgets the back-pointer fails here even if no case above names it.
    int32 Orphans = 0;
    for (const TObjectPtr<UMaterialExpression>& Expr : Material->GetExpressions())
    {
        if (Expr && static_cast<UMaterial*>(Expr->Material) != Material)
        {
            ++Orphans;
            AddError(FString::Printf(
                TEXT("%s was created with a Material back-pointer of %s; expected the owning material."),
                *Expr->GetClass()->GetName(),
                Expr->Material ? *Expr->Material->GetPathName() : TEXT("null")));
        }
    }
    TestEqual(TEXT("No created expression is left without its owning-material back-pointer"), Orphans, 0);

    return true;
}


// Function graph: an expression owned by a UMaterialFunction forwards its edits through Function,
// not Material — UMaterialExpression::PostEditChangeProperty only reaches Function when Material is
// null, which is the pair the material editor itself writes back when it saves a function graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialExpressionOwnerBackPointerFunctionTest,
    "PinWright.material.authoring.expression_back_pointer.CreatedExpressionsResolveOwningFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialExpressionOwnerBackPointerFunctionTest::RunTest(const FString& Parameters)
{
    const FString FuncName = FString::Printf(TEXT("MF_BackPtrOwner_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString FuncFolder = TEXT("/Game/__PW_GatewayTests");
    const FString FuncPackagePath = FString::Printf(TEXT("%s/%s"), *FuncFolder, *FuncName);
    const FString FuncObjectPath = FString::Printf(TEXT("%s.%s"), *FuncPackagePath, *FuncName);
    ON_SCOPE_EXIT { CleanupTestAsset(FuncPackagePath); };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FuncName);
        Payload->SetStringField(TEXT("path"), FuncFolder);
        Payload->SetBoolField(TEXT("save"), false);
        InvokeCreateVerb(*this, TEXT("material.authoring.create_material_function"), Payload);
    }

    UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *FuncObjectPath);
    if (!TestNotNull(TEXT("Fixture material function created"), Function))
        return true;

    // Direct NewObject sites in MaterialAuthoringHandler, plus the factory's function overload.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Brightness"));
        Payload->SetStringField(TEXT("inputType"), TEXT("Scalar"));
        Payload->SetNumberField(TEXT("x"), -400.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        InvokeCreateVerb(*this, TEXT("material.authoring.add_function_input"), Payload);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Result"));
        Payload->SetNumberField(TEXT("x"), 200.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        InvokeCreateVerb(*this, TEXT("material.authoring.add_function_output"), Payload);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
        Payload->SetNumberField(TEXT("x"), -200.0);
        Payload->SetNumberField(TEXT("y"), 120.0);
        InvokeCreateVerb(*this, TEXT("material.graph.add_expression"), Payload);
    }

    int32 Checked = 0;
    for (const TObjectPtr<UMaterialExpression>& Expr : Function->GetExpressions())
    {
        if (!Expr)
            continue;
        ++Checked;
        TestTrue(*FString::Printf(TEXT("%s carries its owning-function back-pointer"),
                *Expr->GetClass()->GetName()),
            static_cast<UMaterialFunction*>(Expr->Function) == Function);
        // Material must stay null on a function-owned node: a non-null Material short-circuits the
        // engine's forward before it ever reaches Function.
        TestNull(*FString::Printf(TEXT("%s leaves Material null so the Function forward is reached"),
                *Expr->GetClass()->GetName()),
            static_cast<UMaterial*>(Expr->Material));
    }
    TestTrue(TEXT("The function received the expressions the verbs reported creating"), Checked >= 3);

    return true;
}
