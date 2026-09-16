// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Handlers/Material/MaterialFinders.h"
#include "Material/MaterialExpressionFactory.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace PinWrightStaticSwitchFunctionOutputPersistence
{
    bool Invoke(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload, FString* OutNodeId = nullptr)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s registered"), Method), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s delivered a response"), Method), Capture.bWasCalled);
        Test.TestTrue(*FString::Printf(TEXT("%s succeeded: %s %s"),
            Method, *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        if (!bFound || !Capture.bWasCalled || !Capture.bSuccess)
        {
            return false;
        }
        if (OutNodeId)
        {
            if (Capture.Result.IsValid())
            {
                Capture.Result->TryGetStringField(TEXT("nodeId"), *OutNodeId);
            }
            return Test.TestFalse(TEXT("Creation returned a nodeId"), OutNodeId->IsEmpty());
        }
        return true;
    }

    TSharedPtr<FJsonObject> NodePayload(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetNumberField(TEXT("x"), 200);
        Payload->SetNumberField(TEXT("y"), 0);
        return Payload;
    }

    bool Save(FAutomationTestBase& Test, UObject* Asset)
    {
        const FString Filename = PackageFilenameFromAssetPath(Asset->GetOutermost()->GetName());
        if (!Test.TestTrue(TEXT("Package has a disk filename"), !Filename.IsEmpty()))
        {
            return false;
        }
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args);
        Test.TestTrue(TEXT("Fixture package saved"), bSaved);
        const bool bHasBytes = IFileManager::Get().FileSize(*Filename) > 0;
        Test.TestTrue(TEXT("Saved fixture has bytes on disk"), bHasBytes);
        return bSaved && bHasBytes;
    }

    void RunFixture(FAutomationTestBase& Test, bool bGenericOutput)
    {
        const FString FixtureGuid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Folder = TEXT("/Game/PinWrightTests/") + FixtureGuid;
        const FString FunctionPackagePath = Folder + TEXT("/MF_Source");
        const FString MaterialPackagePath = Folder + TEXT("/M_Consumer");
        const FString FunctionObjectPath = FunctionPackagePath + TEXT(".MF_Source");
        const FString MaterialObjectPath = MaterialPackagePath + TEXT(".M_Consumer");
        TStrongObjectPtr<UMaterialFunction> Function;
        TStrongObjectPtr<UMaterial> Material;
        ON_SCOPE_EXIT
        {
            Material.Reset();
            Function.Reset();
            CleanupTestAsset(MaterialPackagePath);
            CleanupTestAsset(FunctionPackagePath);

            // Remove only this GUID directory, and only if empty. Never recurse into the shared root.
            FString FixtureFilename;
            FString RootFilename;
            if (FPackageName::TryConvertLongPackageNameToFilename(MaterialPackagePath, FixtureFilename)
                && FPackageName::TryConvertLongPackageNameToFilename(
                    FString(TEXT("/Game/PinWrightTests")), RootFilename))
            {
                const FString FixtureDirectory = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FixtureFilename));
                const FString TestRoot = FPaths::ConvertRelativePathToFull(RootFilename);
                if (FPaths::IsSamePath(FPaths::GetPath(FixtureDirectory), TestRoot)
                    && FPaths::GetCleanFilename(FixtureDirectory) == FixtureGuid)
                {
                    IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, false);
                }
            }
        };

        UPackage* FunctionPackage = CreatePackage(*FunctionPackagePath);
        UPackage* MaterialPackage = CreatePackage(*MaterialPackagePath);
        if (!Test.TestNotNull(TEXT("Function package created"), FunctionPackage)
            || !Test.TestNotNull(TEXT("Material package created"), MaterialPackage))
        {
            return;
        }
        Function.Reset(NewObject<UMaterialFunction>(FunctionPackage, TEXT("MF_Source"), RF_Public | RF_Standalone));
        Material.Reset(NewObject<UMaterial>(MaterialPackage, TEXT("M_Consumer"), RF_Public | RF_Standalone));
        if (!Test.TestNotNull(TEXT("Function created"), Function.Get())
            || !Test.TestNotNull(TEXT("Material created"), Material.Get()))
        {
            return;
        }
        FAssetRegistryModule::AssetCreated(Function.Get());
        FAssetRegistryModule::AssetCreated(Material.Get());

        TSharedPtr<FJsonObject> Payload = NodePayload(FunctionObjectPath);
        Payload->SetStringField(bGenericOutput ? TEXT("nodeType") : TEXT("inputName"),
            bGenericOutput ? TEXT("FunctionOutput") : TEXT("Result"));
        FString OutputNodeId;
        if (!Invoke(Test, bGenericOutput ? TEXT("material.graph.add_node")
            : TEXT("material.authoring.add_function_output"), Payload, &OutputNodeId))
        {
            return;
        }
        UMaterialExpressionFunctionOutput* Output =
            Cast<UMaterialExpressionFunctionOutput>(FindExpressionByIdOrName(Function.Get(), OutputNodeId));
        if (!Test.TestNotNull(TEXT("Output is in the owning function"), Output))
        {
            return;
        }
        // Direct reversion detector: setup must never initialize or replace this ID itself.
        Test.TestTrue(TEXT("Production-created output immediately has a persistent ID"), Output->Id.IsValid());
        const FGuid SavedOutputId = Output->Id;
        UMaterialExpressionConstant3Vector* FunctionConstant =
            NewObject<UMaterialExpressionConstant3Vector>(Function.Get());
        FunctionConstant->Function = Function.Get();
        FunctionConstant->Constant = FLinearColor(0.25f, 0.5f, 0.75f);
        Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(FunctionConstant);
        Output->A.Connect(0, FunctionConstant);

        Payload = NodePayload(MaterialObjectPath);
        Payload->SetStringField(TEXT("functionPath"), FunctionObjectPath);
        FString CallNodeId;
        if (!Invoke(Test, TEXT("material.authoring.use_material_function"), Payload, &CallNodeId))
        {
            return;
        }
        UMaterialExpressionMaterialFunctionCall* Call =
            Cast<UMaterialExpressionMaterialFunctionCall>(FindExpressionByIdOrName(Material.Get(), CallNodeId));
        if (!Test.TestNotNull(TEXT("Call is in the consumer"), Call)
            || !Test.TestEqual(TEXT("Call exposes one output"), Call->FunctionOutputs.Num(), 1))
        {
            return;
        }
        Test.TestEqual(TEXT("Caller cached the persistent output ID"), Call->FunctionOutputs[0].ExpressionOutputId, SavedOutputId);

        UMaterialExpressionStaticSwitchParameter* Switch =
            NewObject<UMaterialExpressionStaticSwitchParameter>(Material.Get());
        UMaterialExpressionConstant3Vector* Constant =
            NewObject<UMaterialExpressionConstant3Vector>(Material.Get());
        Switch->Material = Material.Get();
        Switch->ParameterName = TEXT("UseFunction");
        Switch->DefaultValue = true;
        Constant->Material = Material.Get();
        Constant->Constant = FLinearColor(0.1f, 0.2f, 0.3f);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Switch);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Constant);
        // Keep a complete graph during handler edit notifications; A is replaced post-construction.
        Switch->A.Connect(0, Constant);
        Switch->B.Connect(0, Constant);
        Material->GetEditorOnlyData()->BaseColor.Connect(0, Switch);
        const FString SwitchNodeId = Switch->MaterialExpressionGuid.ToString();
        const FString ConstantNodeId = Constant->MaterialExpressionGuid.ToString();
        Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), CallNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), SwitchNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("A"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 0);
        if (!Invoke(Test, TEXT("material.authoring.connect_nodes"), Payload))
        {
            return;
        }
        Payload->SetStringField(TEXT("sourceNodeId"), ConstantNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("B"));
        if (!Invoke(Test, TEXT("material.graph.connect_nodes"), Payload))
        {
            return;
        }
        Test.TestTrue(TEXT("A is connected to the function call"), Switch->A.Expression == Call);
        Test.TestEqual(TEXT("A selects output zero"), Switch->A.OutputIndex, 0);
        Test.TestTrue(TEXT("B is connected to the control constant"), Switch->B.Expression == Constant);
        if (bGenericOutput)
        {
            Test.TestTrue(TEXT("Reporter engine bypass connects True"),
                UMaterialEditingLibrary::ConnectMaterialExpressions(Call, TEXT(""), Switch, TEXT("True")));
            Test.TestTrue(TEXT("Engine bypass writes A"), Switch->A.Expression == Call);
            Test.TestEqual(TEXT("Engine bypass preserves output zero"), Switch->A.OutputIndex, 0);
        }

        // Save the dependency and consumer while the original live caller table still exists.
        if (!Save(Test, Function.Get()) || !Save(Test, Material.Get()))
        {
            return;
        }
        const TArray<TWeakObjectPtr<UObject>> FormerObjects =
            { Function.Get(), Material.Get(), Output, Call, Switch, Constant };
        const TArray<FString> FormerPaths = { FunctionObjectPath, MaterialObjectPath,
            Output->GetPathName(), Call->GetPathName(), Switch->GetPathName(), Constant->GetPathName() };
        TArray<UPackage*> Packages = { Material->GetOutermost(), Function->GetOutermost() };
        Material.Reset();
        Function.Reset();
        // Native unload drains compilation/rendering before ResetLoaders and GC. ResetLoaders alone
        // would leave live objects behind and turn LoadObject into a resident-memory lookup.
        FText UnloadError;
        bool bEvicted = UPackageTools::UnloadPackages(Packages, UnloadError, false);
        Test.TestTrue(*FString::Printf(TEXT("Both fixture packages unloaded: %s"), *UnloadError.ToString()), bEvicted);
        Packages.Reset();
        for (int32 Index = 0; Index < FormerObjects.Num(); ++Index)
        {
            bEvicted &= Test.TestFalse(TEXT("Former object is no longer resident"), FormerObjects[Index].IsValid());
            bEvicted &= Test.TestNull(TEXT("Former path cannot resolve before disk load"),
                FindObject<UObject>(nullptr, *FormerPaths[Index]));
        }
        if (!bEvicted)
        {
            return;
        }

        Material.Reset(LoadObject<UMaterial>(nullptr, *MaterialObjectPath));
        if (!Test.TestNotNull(TEXT("Consumer reloaded from disk"), Material.Get()))
        {
            return;
        }
        Function.Reset(FindObject<UMaterialFunction>(nullptr, *FunctionObjectPath));
        if (!Test.TestNotNull(TEXT("Consumer load loaded its function dependency"), Function.Get()))
        {
            return;
        }
        // Reacquire by saved expression GUIDs; never read any pre-unload raw pointer again.
        Output = Cast<UMaterialExpressionFunctionOutput>(FindExpressionByIdOrName(Function.Get(), OutputNodeId));
        Call = Cast<UMaterialExpressionMaterialFunctionCall>(FindExpressionByIdOrName(Material.Get(), CallNodeId));
        Switch = Cast<UMaterialExpressionStaticSwitchParameter>(FindExpressionByIdOrName(Material.Get(), SwitchNodeId));
        Constant = Cast<UMaterialExpressionConstant3Vector>(FindExpressionByIdOrName(Material.Get(), ConstantNodeId));
        if (!Test.TestNotNull(TEXT("Reloaded output found in function"), Output)
            || !Test.TestNotNull(TEXT("Reloaded call found in material"), Call)
            || !Test.TestNotNull(TEXT("Reloaded switch found in material"), Switch)
            || !Test.TestNotNull(TEXT("Reloaded control found in material"), Constant)
            || !Test.TestEqual(TEXT("Reloaded call exposes one output"), Call->FunctionOutputs.Num(), 1))
        {
            return;
        }
        Test.TestTrue(TEXT("Reloaded output ID remains valid"), Output->Id.IsValid());
        Test.TestEqual(TEXT("Function output ID survives disk round trip"), Output->Id, SavedOutputId);
        Test.TestEqual(TEXT("Reloaded caller retains the same output ID"), Call->FunctionOutputs[0].ExpressionOutputId, SavedOutputId);
        Test.TestTrue(TEXT("Transient output pointer reconstructed"), Call->FunctionOutputs[0].ExpressionOutput == Output);
        Test.TestTrue(TEXT("Reloaded A still references the reloaded call"), Switch->A.Expression == Call);
        Test.TestEqual(TEXT("Reloaded A output index survives"), Switch->A.OutputIndex, 0);
        Test.TestTrue(TEXT("Reloaded B still references the reloaded control"), Switch->B.Expression == Constant);
        Test.TestEqual(TEXT("Reloaded B output index survives"), Switch->B.OutputIndex, 0);

        if (bGenericOutput)
        {
            Payload = NodePayload(MaterialObjectPath);
            Payload->SetStringField(TEXT("nodeType"), TEXT("FunctionOutput"));
            FString MaterialOutputNodeId;
            if (!Invoke(Test, TEXT("material.graph.add_node"), Payload, &MaterialOutputNodeId))
            {
                return;
            }
            UMaterialExpressionFunctionOutput* MaterialOutput =
                Cast<UMaterialExpressionFunctionOutput>(FindExpressionByIdOrName(Material.Get(), MaterialOutputNodeId));
            if (Test.TestNotNull(TEXT("Material factory branch created an output"), MaterialOutput))
            {
                Test.TestTrue(TEXT("Material-owned output receives a valid ID"), MaterialOutput->Id.IsValid());
            }

            const FGuid ExplicitId = FGuid::NewGuid();
            TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
            Properties->SetStringField(TEXT("Id"), ExplicitId.ToString(EGuidFormats::Digits));
            const FCreateResult Created = FMaterialExpressionFactory::Create(Function.Get(),
                TEXT("FunctionOutput"), Properties, FVector2D(400, 0));
            UMaterialExpressionFunctionOutput* ExplicitOutput = Cast<UMaterialExpressionFunctionOutput>(Created.Expression);
            if (Test.TestNotNull(TEXT("Factory accepts an explicitly supplied output ID"), ExplicitOutput))
            {
                Test.TestEqual(TEXT("Valid supplied output ID is preserved"), ExplicitOutput->Id, ExplicitId);
            }
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticSwitchFunctionOutputPersistenceTest,
    "PinWright.material.authoring.connect_nodes.StaticSwitchFunctionOutputPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStaticSwitchFunctionOutputPersistenceTest::RunTest(const FString& Parameters)
{
    PinWrightStaticSwitchFunctionOutputPersistence::RunFixture(*this, false);
    PinWrightStaticSwitchFunctionOutputPersistence::RunFixture(*this, true);
    return true;
}
