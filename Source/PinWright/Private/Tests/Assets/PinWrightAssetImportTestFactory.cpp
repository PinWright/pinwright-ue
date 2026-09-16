// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/Assets/PinWrightAssetImportTestFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

UPinWrightAssetImportTestFactory::UPinWrightAssetImportTestFactory()
{
    bCreateNew = false;
    bEditorImport = true;
    SupportedClass = UCurveFloat::StaticClass();
    Formats.Add(TEXT("pwmulti;PinWright multi-output automation fixture"));
}

UObject* UPinWrightAssetImportTestFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    (void)Filename;
    (void)Parms;
    (void)Warn;
    bOutOperationCanceled = false;
    ++CreateCallCount;
    AdditionalImportedObjects.Reset();

    UCurveFloat* Primary = NewObject<UCurveFloat>(
        InParent, InClass ? InClass : UCurveFloat::StaticClass(),
        InName, Flags);
    if (!Primary)
    {
        return nullptr;
    }
    Primary->FloatCurve.AddKey(0.0f, 1.0f);
    Primary->GetOutermost()->SetDirtyFlag(true);

    const FString CollateralPackageName =
        InParent->GetOutermost()->GetName() + TEXT("_Collateral");
    UPackage* CollateralPackage = CreatePackage(*CollateralPackageName);
    if (!CollateralPackage)
    {
        return nullptr;
    }
    const FString CollateralName =
        FPackageName::GetLongPackageAssetName(CollateralPackageName);
    UCurveFloat* Collateral = NewObject<UCurveFloat>(
        CollateralPackage, UCurveFloat::StaticClass(), *CollateralName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Collateral)
    {
        return nullptr;
    }
    Collateral->FloatCurve.AddKey(0.0f, 2.0f);
    CollateralPackage->SetDirtyFlag(true);
    FAssetRegistryModule::AssetCreated(Collateral);
    AdditionalImportedObjects.Add(Collateral);
    return Primary;
}
