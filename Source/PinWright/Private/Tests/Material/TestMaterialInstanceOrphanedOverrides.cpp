// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for E-material-instance-info-orphaned-overrides. Synthetic fixture: a parent
// declares two scalar parameters, an instance overrides both, then one parameter is deleted from
// the parent. The instance keeps the dead override (nothing prunes it); get_material_instance_info
// and the shared asset.dump builder must name it in orphanedOverrides and count it, while the live
// override stays out of the list.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Handlers/Asset/MaterialInstanceDumpBuilder.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace TestMaterialInstanceOrphanedOverridesHelpers
{
    UMaterialExpressionScalarParameter* AddScalarParameter(UMaterial* Material, const TCHAR* Name)
    {
        UMaterialExpressionScalarParameter* Param = NewObject<UMaterialExpressionScalarParameter>(
            Material, NAME_None, RF_Transactional);
        Param->Material = Material;
        Param->ParameterName = FName(Name);
        Param->MaterialExpressionGuid = FGuid::NewGuid();
        Param->ExpressionGUID = FGuid::NewGuid();
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Param);
        return Param;
    }

    TArray<FString> OrphanNames(const TSharedPtr<FJsonObject>& Root, const TCHAR* Type)
    {
        TArray<FString> Out;
        const TSharedPtr<FJsonObject>* Orphaned = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
        if (Root.IsValid() && Root->TryGetObjectField(TEXT("orphanedOverrides"), Orphaned) && Orphaned
            && (*Orphaned)->TryGetArrayField(Type, Names) && Names)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Names) Out.Add(Value->AsString());
        }
        return Out;
    }

    int32 OrphanCount(const TSharedPtr<FJsonObject>& Root)
    {
        double Count = -1.0;
        return Root.IsValid() && Root->TryGetNumberField(TEXT("orphanedOverrideCount"), Count) ? static_cast<int32>(Count) : -1;
    }

    TSharedPtr<FJsonObject> ReadInfo(FAutomationTestBase& Test, const FString& InstancePath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("get_material_instance_info is registered"),
            InvokeHandlerWithCapture(TEXT("material.authoring.get_material_instance_info"), Payload, Capture));
        Test.TestTrue(TEXT("get_material_instance_info succeeds"), Capture.bSuccess);
        return Capture.bSuccess ? Capture.Result : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceOrphanedOverridesTest,
    "PinWright.material.instance.OrphanedOverridesAreNamedAndCounted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceOrphanedOverridesTest::RunTest(const FString& Parameters)
{
    using namespace TestMaterialInstanceOrphanedOverridesHelpers;

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ParentPath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/OrphanParent_%s"), *Suffix);
    const FString InstancePath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/OrphanInstance_%s"), *Suffix);
    ON_SCOPE_EXIT { CleanupTestAsset(InstancePath); CleanupTestAsset(ParentPath); };

    UMaterial* Parent = NewObject<UMaterial>(CreatePackage(*ParentPath),
        FName(*FPackageName::GetLongPackageAssetName(ParentPath)), RF_Public | RF_Standalone | RF_Transactional);
    if (!TestNotNull(TEXT("parent material created"), Parent) || !Parent->GetEditorOnlyData()) return true;
    AddScalarParameter(Parent, TEXT("LiveScalar"));
    UMaterialExpressionScalarParameter* Doomed = AddScalarParameter(Parent, TEXT("DoomedScalar"));
    Parent->PostEditChange();
    FAssetRegistryModule::AssetCreated(Parent);

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;
    UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Factory->FactoryCreateNew(
        UMaterialInstanceConstant::StaticClass(), CreatePackage(*InstancePath),
        FName(*FPackageName::GetLongPackageAssetName(InstancePath)), RF_Public | RF_Standalone | RF_Transactional,
        nullptr, GWarn));
    if (!TestNotNull(TEXT("instance created"), Instance)) return true;
    FAssetRegistryModule::AssetCreated(Instance);
    Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LiveScalar")), 0.7f);
    Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("DoomedScalar")), 0.3f);
    if (!TestEqual(TEXT("precondition: the instance overrides both parameters"), Instance->ScalarParameterValues.Num(), 2)) return true;

    // Clean control: both overrides are live.
    {
        const TSharedPtr<FJsonObject> Info = ReadInfo(*this, Instance->GetPathName());
        TestEqual(TEXT("no orphans while the parent declares both"), OrphanCount(Info), 0);
        TestEqual(TEXT("orphanedOverrides.scalar is empty"), OrphanNames(Info, TEXT("scalar")).Num(), 0);
    }

    // The parent drops one parameter through the graph verb; the instance's override survives it.
    TSharedPtr<FJsonObject> Remove = MakeShared<FJsonObject>();
    Remove->SetStringField(TEXT("assetPath"), Parent->GetPathName());
    Remove->SetStringField(TEXT("nodeId"), Doomed->MaterialExpressionGuid.ToString());
    FTestResponseCapture RemoveCapture;
    InvokeHandlerWithCapture(TEXT("material.graph.remove_node"), Remove, RemoveCapture);
    if (!TestTrue(TEXT("precondition: the parent parameter was removed"), RemoveCapture.bSuccess)) return true;
    TestEqual(TEXT("precondition: the dead override is still serialized on the instance"),
        Instance->ScalarParameterValues.Num(), 2);

    const TSharedPtr<FJsonObject> Info = ReadInfo(*this, Instance->GetPathName());
    if (!Info.IsValid()) return true;
    const TArray<FString> Scalars = OrphanNames(Info, TEXT("scalar"));
    TestEqual(TEXT("exactly one scalar override is orphaned"), Scalars.Num(), 1);
    TestTrue(TEXT("the orphan is the parameter the parent deleted"), Scalars.Contains(TEXT("DoomedScalar")));
    TestFalse(TEXT("the live override is not reported as orphaned"), Scalars.Contains(TEXT("LiveScalar")));
    for (const TCHAR* Type : { TEXT("vector"), TEXT("texture"), TEXT("staticSwitch"), TEXT("staticComponentMask") })
    {
        TestEqual(*FString::Printf(TEXT("orphanedOverrides.%s is present and empty"), Type), OrphanNames(Info, Type).Num(), 0);
    }
    TestEqual(TEXT("orphanedOverrideCount counts the dead override"), OrphanCount(Info), 1);

    // overrides itself is unchanged: the dead entry still reads there, which is why the flag exists.
    const TSharedPtr<FJsonObject>* Overrides = nullptr;
    const TSharedPtr<FJsonObject>* OverrideScalars = nullptr;
    TestTrue(TEXT("overrides.scalar still lists the dead override"),
        Info->TryGetObjectField(TEXT("overrides"), Overrides) && Overrides
        && (*Overrides)->TryGetObjectField(TEXT("scalar"), OverrideScalars) && OverrideScalars
        && (*OverrideScalars)->HasField(TEXT("DoomedScalar")));

    // asset.dump's material_instance.json comes from the same builder: same verdict.
    const TSharedPtr<FJsonObject> Dump = MaterialInstanceDumpBuilder::BuildMaterialInstanceJson(Instance);
    TestEqual(TEXT("the dump sidecar carries the same orphanedOverrideCount"), OrphanCount(Dump), 1);
    TestTrue(TEXT("the dump sidecar names the same orphan"), OrphanNames(Dump, TEXT("scalar")).Contains(TEXT("DoomedScalar")));
    return true;
}
