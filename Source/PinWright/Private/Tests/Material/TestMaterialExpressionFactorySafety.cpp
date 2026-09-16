// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-material-abstract-expression. The factory must reject a class that
// cannot be instantiated before it reflects caller properties or reaches NewObject. The handler
// cases below exercise the production RPC paths that share the factory.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/Material/MaterialTestHelpers.h"
#include "Templates/Function.h"

#include "Handlers/ErrorCodes.h"
#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialFunction.h"
#include "UObject/Class.h"

namespace PinWrightMaterialExpressionFactorySafety
{
    struct FClassFlagsGuard
    {
        UClass* Class = nullptr;
        EClassFlags OriginalFlags = CLASS_None;

        explicit FClassFlagsGuard(UClass* InClass)
            : Class(InClass)
            , OriginalFlags(InClass->ClassFlags)
        {
        }

        ~FClassFlagsGuard()
        {
            if (Class)
            {
                Class->ClassFlags = OriginalFlags;
            }
        }
    };

    TSharedPtr<FJsonObject> MakeAddNodePayload(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeType"), TEXT("Constant"));
        Payload->SetNumberField(TEXT("x"), 10.0);
        Payload->SetNumberField(TEXT("y"), 20.0);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeAddExpressionPayload(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("materialPath"), AssetPath);
        Payload->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
        Payload->SetNumberField(TEXT("x"), 30.0);
        Payload->SetNumberField(TEXT("y"), 40.0);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeCreateNodesPayload(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("type"), TEXT("Constant"));
        Node->SetNumberField(TEXT("x"), 50.0);
        Node->SetNumberField(TEXT("y"), 60.0);

        TArray<TSharedPtr<FJsonValue>> Nodes;
        Nodes.Add(MakeShared<FJsonValueObject>(Node));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("materialPath"), AssetPath);
        Payload->SetArrayField(TEXT("nodes"), Nodes);
        return Payload;
    }

    void AssertTypedHandlerRejection(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload, int32 ExpectedExpressionCount,
        TFunctionRef<int32()> GetExpressionCount)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(*FString::Printf(TEXT("%s handler is registered"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("%s responded"), Method), Capture.bWasCalled);
        Test.TestFalse(*FString::Printf(TEXT("%s rejects the non-instantiable class"), Method),
            Capture.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s returns CLASS_NOT_INSTANTIABLE"), Method),
            Capture.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE));
        Test.TestTrue(*FString::Printf(TEXT("%s error names the resolved class"), Method),
            Capture.Message.Contains(TEXT("MaterialExpressionConstant")));
        Test.TestEqual(*FString::Printf(TEXT("%s leaves the expression collection unchanged"), Method),
            GetExpressionCount(), ExpectedExpressionCount);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialExpressionFactoryRejectsNonInstantiableClassesTest,
    "PinWright.material.graph.factory.RejectsNonInstantiableExpressionClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialExpressionFactoryRejectsNonInstantiableClassesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMaterialExpressionFactorySafety;
    using namespace PinWrightMaterialTestHelpers;
    FAutomationTestBase& Test = *this;

    FString MaterialPackagePath;
    UMaterial* Material = CreateFixtureAsset<UMaterial>(*this, TEXT("FactoryFlagsMaterial"),
        MaterialPackagePath);
    if (!Material)
    {
        return true;
    }

    FString FunctionPackagePath;
    UMaterialFunction* Function = CreateFixtureAsset<UMaterialFunction>(*this,
        TEXT("FactoryFlagsFunction"), FunctionPackagePath);
    if (!Function)
    {
        CleanupTestAsset(MaterialPackagePath);
        return true;
    }

    const TArray<EClassFlags> RejectionFlags = {
        CLASS_Abstract,
        CLASS_Deprecated,
        CLASS_NewerVersionExists,
    };
    UClass* ExpressionClass = UMaterialExpressionConstant::StaticClass();

    for (const EClassFlags RejectionFlag : RejectionFlags)
    {
        FClassFlagsGuard Guard(ExpressionClass);
        ExpressionClass->ClassFlags = static_cast<EClassFlags>(
            ExpressionClass->ClassFlags | RejectionFlag);

        Test.TestEqual(TEXT("ResolveExpressionClass still resolves flagged classes"),
            FMaterialExpressionFactory::ResolveExpressionClass(TEXT("Constant")), ExpressionClass);

        TSharedPtr<FJsonObject> InvalidProperties = MakeShared<FJsonObject>();
        InvalidProperties->SetNumberField(TEXT("PropertyThatDoesNotExist"), 1.0);

        const int32 MaterialCountBefore = ExpressionCount(Material);
        FCreateResult MaterialResult = FMaterialExpressionFactory::Create(
            Material, ExpressionClass, InvalidProperties, FVector2D(0.0, 0.0));
        Test.TestFalse(TEXT("Material creation rejects the flagged class"), MaterialResult.IsSuccess());
        Test.TestEqual(TEXT("Material creation returns CLASS_NOT_INSTANTIABLE before property validation"),
            MaterialResult.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE));
        Test.TestTrue(TEXT("Material error identifies the expression class"),
            MaterialResult.ErrorMessage.Contains(TEXT("MaterialExpressionConstant")));
        Test.TestEqual(TEXT("Material collection is unchanged after rejection"),
            ExpressionCount(Material), MaterialCountBefore);

        const int32 FunctionCountBefore = ExpressionCount(Function);
        FCreateResult FunctionResult = FMaterialExpressionFactory::Create(
            Function, ExpressionClass, InvalidProperties, FVector2D(0.0, 0.0));
        Test.TestFalse(TEXT("Material-function creation rejects the flagged class"),
            FunctionResult.IsSuccess());
        Test.TestEqual(TEXT("Material-function creation returns CLASS_NOT_INSTANTIABLE before property validation"),
            FunctionResult.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE));
        Test.TestTrue(TEXT("Material-function error identifies the expression class"),
            FunctionResult.ErrorMessage.Contains(TEXT("MaterialExpressionConstant")));
        Test.TestEqual(TEXT("Material-function collection is unchanged after rejection"),
            ExpressionCount(Function), FunctionCountBefore);
    }

    CleanupTestAsset(MaterialPackagePath);
    CleanupTestAsset(FunctionPackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialExpressionFactoryHandlersRejectNonInstantiableClassesTest,
    "PinWright.material.graph.factory.HandlersRejectNonInstantiableExpressionClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialExpressionFactoryHandlersRejectNonInstantiableClassesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMaterialExpressionFactorySafety;
    using namespace PinWrightMaterialTestHelpers;

    FString MaterialPackagePath;
    UMaterial* Material = CreateFixtureAsset<UMaterial>(*this, TEXT("FactoryHandlersMaterial"),
        MaterialPackagePath);
    if (!Material)
    {
        return true;
    }

    FString FunctionPackagePath;
    UMaterialFunction* Function = CreateFixtureAsset<UMaterialFunction>(*this,
        TEXT("FactoryHandlersFunction"), FunctionPackagePath);
    if (!Function)
    {
        CleanupTestAsset(MaterialPackagePath);
        return true;
    }

    UClass* ExpressionClass = UMaterialExpressionConstant::StaticClass();
    FClassFlagsGuard Guard(ExpressionClass);
    ExpressionClass->ClassFlags = static_cast<EClassFlags>(
        ExpressionClass->ClassFlags | CLASS_Abstract);

    const FString MaterialPath = Material->GetPathName();
    const FString FunctionPath = Function->GetPathName();
    const int32 MaterialCountBefore = ExpressionCount(Material);
    const int32 FunctionCountBefore = ExpressionCount(Function);

    AssertTypedHandlerRejection(*this, TEXT("material.graph.add_node"),
        MakeAddNodePayload(MaterialPath), MaterialCountBefore,
        [Material]() { return PinWrightMaterialTestHelpers::ExpressionCount(Material); });
    AssertTypedHandlerRejection(*this, TEXT("material.graph.add_expression"),
        MakeAddExpressionPayload(MaterialPath), MaterialCountBefore,
        [Material]() { return PinWrightMaterialTestHelpers::ExpressionCount(Material); });
    AssertTypedHandlerRejection(*this, TEXT("material.graph.add_node"),
        MakeAddNodePayload(FunctionPath), FunctionCountBefore,
        [Function]() { return PinWrightMaterialTestHelpers::ExpressionCount(Function); });
    AssertTypedHandlerRejection(*this, TEXT("material.graph.add_expression"),
        MakeAddExpressionPayload(FunctionPath), FunctionCountBefore,
        [Function]() { return PinWrightMaterialTestHelpers::ExpressionCount(Function); });

    FTestResponseCapture BatchCapture;
    TestTrue(TEXT("material.graph.create_nodes handler is registered"),
        InvokeHandlerWithCapture(TEXT("material.graph.create_nodes"),
            MakeCreateNodesPayload(MaterialPath), BatchCapture));
    TestTrue(TEXT("material.graph.create_nodes responds"), BatchCapture.bWasCalled);
    TestTrue(TEXT("material.graph.create_nodes reports the rejected node in failCount"),
        BatchCapture.bSuccess);
    int32 SuccessCount = -1;
    int32 FailCount = -1;
    if (BatchCapture.Result.IsValid())
    {
        BatchCapture.Result->TryGetNumberField(TEXT("successCount"), SuccessCount);
        BatchCapture.Result->TryGetNumberField(TEXT("failCount"), FailCount);
    }
    TestEqual(TEXT("create_nodes reports zero successful nodes"), SuccessCount, 0);
    TestEqual(TEXT("create_nodes reports one rejected node"), FailCount, 1);
    TestEqual(TEXT("create_nodes leaves the expression collection unchanged"),
        ExpressionCount(Material), MaterialCountBefore);

    CleanupTestAsset(MaterialPackagePath);
    CleanupTestAsset(FunctionPackagePath);
    return true;
}
