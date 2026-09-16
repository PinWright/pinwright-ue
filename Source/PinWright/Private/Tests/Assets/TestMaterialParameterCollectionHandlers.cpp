// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for material.authoring.* MaterialParameterCollection (MPC)
// handlers in Handlers/Material/MaterialParameterCollectionHandler.cpp.
//
// Round-trip covers:
//   create_parameter_collection
//   add_collection_scalar_parameter
//   add_collection_vector_parameter
//   set_collection_parameter_default
//   remove_collection_parameter
//   get_parameter_collection_info
//   add_collection_parameter_node — the atomic Collection+ParameterName+ParameterId
//                                   bind on a UMaterialExpressionCollectionParameter.
//                                   If ParameterId is left at the default FGuid the
//                                   shader compiler reports "(Invalid Parameter)";
//                                   this test guards against that regression by
//                                   asserting the node's ParameterId equals the one
//                                   returned by Collection->GetParameterId(name).

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"

namespace
{
    UMaterial* CreateTransientHostMaterial(const FString& NamePrefix)
    {
        const FString AssetName = FString::Printf(TEXT("%s_%s"),
            *NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), FName(*AssetName),
            RF_Public | RF_Transient);
        if (Material)
        {
            Material->AddToRoot();
        }
        return Material;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringParameterCollectionRoundTrip,
    "PinWright.Material.Authoring.ParameterCollectionRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringParameterCollectionRoundTrip::RunTest(const FString& Parameters)
{
    // Unique asset names so concurrent test runs don't collide.
    const FString NameSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString CollectionAssetName = FString::Printf(TEXT("MPC_RT_%s"), *NameSuffix);
    const FString CollectionPackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *CollectionAssetName);
    const FString CollectionObjectPath = FString::Printf(
        TEXT("%s.%s"), *CollectionPackagePath, *CollectionAssetName);

    // Always clean up the persistent MPC asset, even on early returns.
    UMaterial* HostMaterial = nullptr;
    ON_SCOPE_EXIT
    {
        if (HostMaterial)
        {
            HostMaterial->RemoveFromRoot();
        }
        CleanupTestAsset(CollectionPackagePath);
    };

    // ---- create_parameter_collection ------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), CollectionAssetName);
        Payload->SetStringField(TEXT("path"), TEXT("/Game/PinWrightTests"));
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("create_parameter_collection handler found"),
            InvokeHandler(TEXT("material.authoring.create_parameter_collection"), Payload));
    }

    UMaterialParameterCollection* Collection =
        LoadObject<UMaterialParameterCollection>(nullptr, *CollectionObjectPath);
    TestNotNull(TEXT("MPC asset created"), Collection);
    if (!Collection) return true;

    // ---- add_collection_scalar_parameter --------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), CollectionObjectPath);
        Payload->SetStringField(TEXT("name"), TEXT("MyScalar"));
        Payload->SetNumberField(TEXT("default"), 0.5);
        TestTrue(TEXT("add_collection_scalar_parameter handler found"),
            InvokeHandler(TEXT("material.authoring.add_collection_scalar_parameter"), Payload));
    }

    TestEqual(TEXT("collection has one scalar parameter"),
        Collection->ScalarParameters.Num(), 1);
    if (Collection->ScalarParameters.Num() != 1) return true;
    TestEqual(TEXT("scalar parameter name"),
        Collection->ScalarParameters[0].ParameterName, FName(TEXT("MyScalar")));
    TestEqual(TEXT("scalar parameter default"),
        Collection->ScalarParameters[0].DefaultValue, 0.5f);
    TestTrue(TEXT("scalar parameter Id is non-default"),
        Collection->ScalarParameters[0].Id.IsValid());
    const FGuid ScalarId = Collection->ScalarParameters[0].Id;

    // ---- add_collection_vector_parameter --------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), CollectionObjectPath);
        Payload->SetStringField(TEXT("name"), TEXT("MyVector"));

        TSharedPtr<FJsonObject> DefaultObj = MakeShared<FJsonObject>();
        DefaultObj->SetNumberField(TEXT("R"), 0.25);
        DefaultObj->SetNumberField(TEXT("G"), 0.5);
        DefaultObj->SetNumberField(TEXT("B"), 0.75);
        DefaultObj->SetNumberField(TEXT("A"), 1.0);
        Payload->SetObjectField(TEXT("default"), DefaultObj);

        TestTrue(TEXT("add_collection_vector_parameter handler found"),
            InvokeHandler(TEXT("material.authoring.add_collection_vector_parameter"), Payload));
    }

    TestEqual(TEXT("collection has one vector parameter"),
        Collection->VectorParameters.Num(), 1);
    if (Collection->VectorParameters.Num() != 1) return true;
    TestEqual(TEXT("vector parameter name"),
        Collection->VectorParameters[0].ParameterName, FName(TEXT("MyVector")));
    TestEqual(TEXT("vector default R"), Collection->VectorParameters[0].DefaultValue.R, 0.25f);
    TestEqual(TEXT("vector default G"), Collection->VectorParameters[0].DefaultValue.G, 0.5f);
    TestEqual(TEXT("vector default B"), Collection->VectorParameters[0].DefaultValue.B, 0.75f);
    TestEqual(TEXT("vector default A"), Collection->VectorParameters[0].DefaultValue.A, 1.0f);
    const FGuid VectorId = Collection->VectorParameters[0].Id;

    // ---- set_collection_parameter_default (scalar) ----------------------
    // The Id must NOT be regenerated by this op or every consumer node is orphaned.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), CollectionObjectPath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("MyScalar"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("scalar"));
        Payload->SetNumberField(TEXT("value"), 1.25);
        TestTrue(TEXT("set_collection_parameter_default handler found"),
            InvokeHandler(TEXT("material.authoring.set_collection_parameter_default"), Payload));
    }
    TestEqual(TEXT("scalar default updated"),
        Collection->ScalarParameters[0].DefaultValue, 1.25f);
    TestEqual(TEXT("scalar Id preserved across default-update"),
        Collection->ScalarParameters[0].Id, ScalarId);

    // ---- get_parameter_collection_info ----------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), CollectionObjectPath);
        FTestResponseCapture Capture;
        TestTrue(TEXT("get_parameter_collection_info handler found"),
            InvokeHandlerWithCapture(TEXT("material.authoring.get_parameter_collection_info"),
                Payload, Capture));
        TestTrue(TEXT("get_info returned success"), Capture.bSuccess);
        TestTrue(TEXT("get_info has result"), Capture.Result.IsValid());
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* ScalarsArray = nullptr;
            TestTrue(TEXT("scalars array present"),
                Capture.Result->TryGetArrayField(TEXT("scalars"), ScalarsArray));
            if (ScalarsArray)
            {
                TestEqual(TEXT("scalars array length"), ScalarsArray->Num(), 1);
                if (ScalarsArray->Num() == 1)
                {
                    const TSharedPtr<FJsonObject> Obj = (*ScalarsArray)[0]->AsObject();
                    TestEqual(TEXT("scalar info name"),
                        Obj->GetStringField(TEXT("name")), FString(TEXT("MyScalar")));
                    TestEqual(TEXT("scalar info default"),
                        Obj->GetNumberField(TEXT("default")), 1.25);
                    TestEqual(TEXT("scalar info parameterId matches asset"),
                        Obj->GetStringField(TEXT("parameterId")), ScalarId.ToString());
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* VectorsArray = nullptr;
            TestTrue(TEXT("vectors array present"),
                Capture.Result->TryGetArrayField(TEXT("vectors"), VectorsArray));
            if (VectorsArray)
            {
                TestEqual(TEXT("vectors array length"), VectorsArray->Num(), 1);
            }
        }
    }

    // ---- add_collection_parameter_node ----------------------------------
    // Critical: the node must hold (Collection, ParameterName, ParameterId) all
    // bound atomically. If the ParameterId line is reverted in the handler, the
    // node ships with a default-constructed FGuid and the renderer reports the
    // node as "(Invalid Parameter)" — the final assertion below guards that.
    HostMaterial = CreateTransientHostMaterial(TEXT("MPC_Host"));
    TestNotNull(TEXT("transient host material created"), HostMaterial);
    if (!HostMaterial) return true;

    const FString HostAssetPath = HostMaterial->GetPathName();
    FString CreatedNodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), HostAssetPath);
        Payload->SetStringField(TEXT("collectionPath"), CollectionObjectPath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("MyScalar"));
        Payload->SetNumberField(TEXT("x"), -320.0);
        Payload->SetNumberField(TEXT("y"),   80.0);
        FTestResponseCapture Capture;
        TestTrue(TEXT("add_collection_parameter_node handler found"),
            InvokeHandlerWithCapture(TEXT("material.authoring.add_collection_parameter_node"),
                Payload, Capture));
        TestTrue(TEXT("add_collection_parameter_node returned success"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            CreatedNodeId = Capture.Result->GetStringField(TEXT("nodeId"));
        }
    }
    TestFalse(TEXT("created node id non-empty"), CreatedNodeId.IsEmpty());

    UMaterialExpressionCollectionParameter* CreatedNode = nullptr;
    for (UMaterialExpression* Expr : HostMaterial->GetExpressions())
    {
        UMaterialExpressionCollectionParameter* Candidate =
            Cast<UMaterialExpressionCollectionParameter>(Expr);
        if (Candidate && Candidate->MaterialExpressionGuid.ToString() == CreatedNodeId)
        {
            CreatedNode = Candidate;
            break;
        }
    }
    TestNotNull(TEXT("collection-parameter node added to material expressions"), CreatedNode);
    if (!CreatedNode) return true;

    // Atomic three-field bind assertions — the foot-gun guard.
    TestEqual(TEXT("node Collection bound"),
        (UMaterialParameterCollection*)CreatedNode->Collection, Collection);
    TestEqual(TEXT("node ParameterName bound"),
        CreatedNode->ParameterName, FName(TEXT("MyScalar")));
    TestEqual(TEXT("node ParameterId matches collection's scalar Id"),
        CreatedNode->ParameterId, ScalarId);
    TestTrue(TEXT("node ParameterId is non-default (foot-gun guard)"),
        CreatedNode->ParameterId.IsValid());

    // ---- remove_collection_parameter ------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), CollectionObjectPath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("MyVector"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("vector"));
        TestTrue(TEXT("remove_collection_parameter handler found"),
            InvokeHandler(TEXT("material.authoring.remove_collection_parameter"), Payload));
    }
    TestEqual(TEXT("vector parameter removed"),
        Collection->VectorParameters.Num(), 0);
    // VectorId is captured but no longer needs to round-trip; the assertion
    // above confirms the array shrank.
    (void)VectorId;

    return true;
}
